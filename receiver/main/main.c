/*
 * LibreCom receiver: framed UART -> Opus -> PCM5102A I2S DAC.
 *
 * The two boards run off separate crystals, so the sender produces samples at
 * a slightly different rate than this board consumes them. A jitter buffer
 * sits between the decoder and the I2S writer to absorb that, and trims or
 * pads by whole frames when the level drifts too far. Without it you get the
 * warble you hear when the DAC keeps replaying a stale DMA buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "opus.h"
#include "audio_link.h"
#include "link_config.h"

/* ------------------------------------------------------------------ *
 *  Bring-up switches
 * ------------------------------------------------------------------ */

/* 1 = ignore the UART completely and play a locally generated 440 Hz tone.
 * That is the "is my DAC wired up at all" test. */
#define RX_LOCAL_TONE    0

/* 1 = loop this board UART1 TX straight back into its own RX inside the chip
 * and feed it locally generated frames. Nothing on the wire is involved.
 *
 * This is the test for "bytes=0": if the loopback gives good=50 then this
 * board UART, the framing, the jitter buffer and the DAC are all proven, and
 * the fault is the transmitter or the cable. If the loopback also gives
 * bytes=0, the problem is on this board. */
#define RX_UART_LOOPBACK 0

/* Frequency of the locally generated tone used by RX_LOCAL_TONE and by the
 * loopback generator. Sweep this to measure the DAC and analogue output
 * response: at 8 kHz everything up to about 3.6 kHz should come out at the
 * same loudness. A tone that gets quiet well below that means the output
 * stage, not the codec, is what sounds muffled. */
#define RX_TONE_HZ       440      /* integer Hz, so the guard below can test it */

/* 1 = step automatically through a list of frequencies, about two seconds
 * each, logging every step. One flash measures the whole response, instead of
 * reflashing once per frequency. 0 = hold RX_TONE_HZ. */
#define RX_TONE_SWEEP    1

/* Neither of the above is compiled in unless a tone mode is active, so
 * changing them would otherwise do nothing at all and say nothing about it.
 * Only complain when the value has actually been changed, so a normal build
 * stays quiet. */
#if !RX_LOCAL_TONE && !RX_UART_LOOPBACK && (RX_TONE_HZ != 440)
#  warning "RX_TONE_HZ has no effect here - set RX_LOCAL_TONE to 1 to hear a tone"
#endif

/* Jitter buffer depth. Bigger = more robust against hiccups, more latency. */
#define JB_TARGET_MS     60      /* level we prebuffer to before playing out */
#define JB_HIGH_MS       140     /* above this we drop a frame to catch up   */

/* How many frames of a gap we paper over by repeating the last one. */
#define PLC_MAX_FRAMES   2

/* Pins */
#define I2S_BCK_IO       (17)    /* -> PCM5102A BCK  */
#define I2S_WS_IO        (18)    /* -> PCM5102A LRCK */
#define I2S_DO_IO        (19)    /* -> PCM5102A DIN  */
#define I2S_MCK_IO       (-1)    /* PCM5102A runs off its internal PLL */

#define UART_PORT        (UART_NUM_1)
#if LINK_SWAP_PINS
#  define UART_TX_IO     (5)
#  define UART_RX_IO     (4)     /* <- transmitter GPIO5, the other jumper */
#else
#  define UART_TX_IO     (4)     /* unused here, but the driver wants a pin */
#  define UART_RX_IO     (5)     /* <- transmitter GPIO4 */
#endif

/* ------------------------------------------------------------------ */

static const char *TAG = "rx";

#define RING_SAMPLES     16384   /* power of two, ~1 s at 16 kHz */
#define JB_TARGET        (LINK_SAMPLE_RATE * JB_TARGET_MS / 1000)
#define JB_HIGH          (LINK_SAMPLE_RATE * JB_HIGH_MS   / 1000)

_Static_assert(LINK_PAYLOAD_MAX <= LINK_MAX_PAYLOAD, "payload too large for audio_link");
_Static_assert(JB_HIGH < RING_SAMPLES - LINK_FRAME_SAMPLES, "jitter buffer will not fit");

static i2s_chan_handle_t s_tx_chan;

/* ------------------------------------------------------------------ *
 *  Single producer / single consumer ring of PCM samples.
 *  Both tasks are pinned to core 1, so plain volatile indices are enough:
 *  each index has exactly one writer, and the data is always stored before
 *  the index that publishes it is advanced.
 * ------------------------------------------------------------------ */
#if !RX_LOCAL_TONE
static int16_t s_ring[RING_SAMPLES];
static volatile uint32_t s_head;   /* written by the decoder */
static volatile uint32_t s_tail;   /* written by the I2S writer */

static inline uint32_t ring_avail(void) { return s_head - s_tail; }
static inline uint32_t ring_space(void) { return RING_SAMPLES - 1 - ring_avail(); }

static void ring_write(const int16_t *src, uint32_t n)
{
    uint32_t h = s_head;
    for (uint32_t i = 0; i < n; i++) {
        s_ring[(h + i) & (RING_SAMPLES - 1)] = src[i];
    }
    s_head = h + n;
}

static void ring_read(int16_t *dst, uint32_t n)
{
    uint32_t t = s_tail;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = s_ring[(t + i) & (RING_SAMPLES - 1)];
    }
    s_tail = t + n;
}
#endif /* !RX_LOCAL_TONE */

/* ------------------------------------------------------------------ *
 *  Statistics, printed once a second by the I2S task
 * ------------------------------------------------------------------ */
static volatile uint32_t st_underruns;
static volatile uint32_t st_drops;
static volatile uint32_t st_concealed;
static volatile uint32_t st_dec_total_us;
static volatile uint32_t st_dec_max_us;
static volatile uint32_t st_decoded;
static volatile uint32_t st_rx_bytes;   /* raw bytes off the wire, per second */
static volatile uint32_t st_rxq_max;    /* peak UART driver backlog           */
#if !RX_LOCAL_TONE
static uint8_t  st_peek[16];            /* first raw bytes, when nothing syncs */
static volatile uint32_t st_peek_len;
#endif
#if !RX_LOCAL_TONE
static link_parser_t     s_parser;
#endif

/* ------------------------------------------------------------------ *
 *  Peripherals
 * ------------------------------------------------------------------ */
static void init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = LINK_FRAME_SAMPLES;   /* 20 ms per descriptor */
    /* Emit silence rather than replaying the previous DMA buffer if we ever
     * fail to keep up. A gap is far less objectionable than a stutter. */
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(LINK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    /* The default MONO slot mask is LEFT, which leaves the right channel of
     * the DAC silent. MONO + BOTH makes the hardware copy each sample into
     * both slots, so either output carries the audio. */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_LOGI(TAG, "I2S TX up at %d Hz, mono duplicated to both slots", LINK_SAMPLE_RATE);
}

#if !RX_LOCAL_TONE
static void init_uart(void)
{
    uart_config_t cfg = {
        .baud_rate  = LINK_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Generous RX ring: roughly a quarter second of link, so a scheduling
     * hiccup costs us latency rather than lost bytes. */
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 4096,
                                        RX_UART_LOOPBACK ? 2048 : 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_IO, UART_RX_IO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#if RX_UART_LOOPBACK
    ESP_ERROR_CHECK(uart_set_loop_back(UART_PORT, true));
    ESP_LOGW(TAG, "UART%d in INTERNAL LOOPBACK at %d baud - the wire is bypassed",
             (int)UART_PORT, LINK_UART_BAUD);
#else
    ESP_LOGI(TAG, "UART%d up at %d baud, RX on GPIO%d",
             (int)UART_PORT, LINK_UART_BAUD, UART_RX_IO);
#endif
}
#endif /* !RX_LOCAL_TONE */

/* ------------------------------------------------------------------ *
 *  Decoder: UART bytes -> PCM in the ring
 * ------------------------------------------------------------------ */
#if !RX_LOCAL_TONE
#if !LINK_SEND_PCM
static OpusDecoder *s_dec;              /* also drives packet loss concealment */
#endif
static int16_t s_last_frame[LINK_FRAME_SAMPLES];   /* for gap concealment */
static bool    s_have_last;

static void push_frame(const int16_t *pcm)
{
    if (ring_space() >= LINK_FRAME_SAMPLES) {
        ring_write(pcm, LINK_FRAME_SAMPLES);
    }
    /* If there is no space the buffer is already over the high water mark and
     * the I2S task is about to trim it, so dropping here is the right move. */
}

/* Fill a detected gap. Opus has real packet loss concealment built in -
 * calling opus_decode() with a NULL packet extrapolates from the decoder's
 * own state, which sounds far better than the attenuated repeat we used with
 * Codec 2 (which had no PLC of its own). */
static void conceal(int frames)
{
    static int16_t tmp[LINK_FRAME_SAMPLES];
    for (int f = 0; f < frames && f < PLC_MAX_FRAMES; f++) {
#if LINK_SEND_PCM
        if (!s_have_last) return;
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            tmp[i] = (int16_t)((float)s_last_frame[i] * 0.6f);
        }
#else
        if (opus_decode(s_dec, NULL, 0, tmp, LINK_FRAME_SAMPLES, 0) != LINK_FRAME_SAMPLES) {
            return;
        }
#endif
        push_frame(tmp);
        st_concealed++;
    }
}

static void net_task(void *arg)
{
    (void)arg;

#if !LINK_SEND_PCM
    int oerr = 0;
    s_dec = opus_decoder_create(LINK_SAMPLE_RATE, 1, &oerr);
    if (!s_dec || oerr != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %s - stopping", opus_strerror(oerr));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Opus %s: %d Hz, %d ms frames",
             opus_get_version_string(), LINK_SAMPLE_RATE, LINK_FRAME_MS);
#endif

    link_parser_init(&s_parser, LINK_PAYLOAD_MAX);

    static uint8_t rxbuf[512];
    static int16_t pcm[LINK_FRAME_SAMPLES];

    /* Anything that piled up while we were booting is stale audio. Start from
     * live data instead of playing a backlog of history. */
    uart_flush_input(UART_PORT);

    while (1) {
        /* Read exactly what is waiting, so a backlog is cleared in one pass
         * rather than trickled out one frame-time at a time. Only when the
         * driver is empty do we block for the next byte. */
        size_t buffered = 0;
        uart_get_buffered_data_len(UART_PORT, &buffered);
        if (buffered > st_rxq_max) {
            st_rxq_max = buffered;
        }
        size_t want = buffered ? buffered : 1;
        if (want > sizeof(rxbuf)) {
            want = sizeof(rxbuf);
        }

        int n = uart_read_bytes(UART_PORT, rxbuf, want, pdMS_TO_TICKS(20));
        if (n > 0) {
            st_rx_bytes += (uint32_t)n;
        }
        /* If nothing has synced yet, keep the first few raw bytes so the log
         * can show whether we are seeing valid framing at the wrong length or
         * simply garbage from a baud mismatch. */
        if (s_parser.n_good == 0 && st_peek_len < sizeof(st_peek)) {
            for (int i = 0; i < n && st_peek_len < sizeof(st_peek); i++) {
                st_peek[st_peek_len++] = rxbuf[i];
            }
        }

        for (int i = 0; i < n; i++) {
            if (!link_parser_feed(&s_parser, rxbuf[i])) {
                continue;
            }

            conceal(s_parser.gap);

            int64_t t0 = esp_timer_get_time();
#if LINK_SEND_PCM
#  if LINK_PCM_ULAW
            for (int k = 0; k < LINK_FRAME_SAMPLES; k++) {
                pcm[k] = link_ulaw_decode(s_parser.payload[k]);
            }
#  else
            memcpy(pcm, s_parser.payload, LINK_FRAME_SAMPLES * sizeof(int16_t));
#  endif
#else
            int got = opus_decode(s_dec, s_parser.payload, s_parser.payload_len,
                                  pcm, LINK_FRAME_SAMPLES, 0);
            if (got != LINK_FRAME_SAMPLES) {
                ESP_LOGW(TAG, "opus_decode returned %d", got);
                memset(pcm, 0, sizeof(pcm));
            }
#endif
            uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            st_dec_total_us += dt;
            if (dt > st_dec_max_us) st_dec_max_us = dt;
            st_decoded++;

            memcpy(s_last_frame, pcm, sizeof(s_last_frame));
            s_have_last = true;
            push_frame(pcm);
        }
    }
}
#endif /* !RX_LOCAL_TONE */

/* ------------------------------------------------------------------ *
 *  I2S writer: drains the ring at the DAC clock, and is the only thing
 *  that decides how deep the jitter buffer runs.
 * ------------------------------------------------------------------ */
static void dac_task(void *arg)
{
    (void)arg;

    static int16_t block[LINK_FRAME_SAMPLES];
#if !RX_LOCAL_TONE
    bool prebuffering = true;
    uint32_t jb_sum = 0, jb_n = 0, jb_min = UINT32_MAX, jb_max = 0;
#endif

#if RX_LOCAL_TONE
    float phase = 0.0f;
#if RX_TONE_SWEEP
    /* Chosen to straddle the 8 kHz band: everything here should come out at
     * about the same loudness. Where it starts fading is your real bandwidth. */
    static const float sweep_hz[] = { 300, 500, 800, 1200, 1600, 2000, 2500, 3000, 3400 };
    const int   n_sweep         = sizeof(sweep_hz) / sizeof(sweep_hz[0]);
    const int   frames_per_step = 2000 / LINK_FRAME_MS;      /* ~2 s per tone */
    int   sweep_i = 0, sweep_frames = 0;
    float inc = 2.0f * (float)M_PI * sweep_hz[0] / (float)LINK_SAMPLE_RATE;
    ESP_LOGI(TAG, "RX_LOCAL_TONE: sweeping %d tones, %d ms each, UART ignored",
             n_sweep, 2000);
    ESP_LOGI(TAG, "  playing %d Hz", (int)sweep_hz[0]);
#else
    const float inc = 2.0f * (float)M_PI * (float)RX_TONE_HZ / (float)LINK_SAMPLE_RATE;
    ESP_LOGI(TAG, "RX_LOCAL_TONE: playing a local %d Hz tone, UART ignored", RX_TONE_HZ);
#endif
#else
    ESP_LOGI(TAG, "jitter buffer: target %d ms, drop above %d ms",
             JB_TARGET_MS, JB_HIGH_MS);
#endif

    int64_t  t_report = esp_timer_get_time();

    while (1) {
#if RX_LOCAL_TONE
#if RX_TONE_SWEEP
        if (++sweep_frames >= frames_per_step) {
            sweep_frames = 0;
            sweep_i = (sweep_i + 1) % n_sweep;
            inc = 2.0f * (float)M_PI * sweep_hz[sweep_i] / (float)LINK_SAMPLE_RATE;
            ESP_LOGI(TAG, "  playing %d Hz", (int)sweep_hz[sweep_i]);
        }
#endif
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            block[i] = (int16_t)(12000.0f * sinf(phase));
            phase += inc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
#else
        uint32_t avail = ring_avail();
        jb_sum += avail;
        jb_n++;
        if (avail < jb_min) jb_min = avail;
        if (avail > jb_max) jb_max = avail;

        bool have_audio = false;

        if (prebuffering && avail >= (uint32_t)JB_TARGET) {
            prebuffering = false;
        }

        if (!prebuffering) {
            /* Sender is running fast (or we hiccuped): throw one frame away
             * so latency does not creep up without bound. */
            if (avail >= (uint32_t)JB_HIGH) {
                s_tail += LINK_FRAME_SAMPLES;
                st_drops++;
                avail  -= LINK_FRAME_SAMPLES;
            }
            if (avail >= LINK_FRAME_SAMPLES) {
                ring_read(block, LINK_FRAME_SAMPLES);
                have_audio = true;
            } else {
                st_underruns++;
                prebuffering = true;   /* refill before trying again */
            }
        }

        if (!have_audio) {
            memset(block, 0, sizeof(block));
        }
#endif
        {
            size_t written = 0;
            i2s_channel_write(s_tx_chan, block, sizeof(block), &written, portMAX_DELAY);
        }

        int64_t now = esp_timer_get_time();
        if (now - t_report >= 1000000) {
#if RX_LOCAL_TONE
            ESP_LOGI(TAG, "local tone running, heap=%u", (unsigned)esp_get_free_heap_size());
#else
            /* Same normalisation as the transmitter: the window is a whole
             * number of 20 ms blocks, so scale to a true per-second rate. */
            int64_t  elapsed = now - t_report;
            uint32_t d = st_decoded; if (!d) d = 1;
            uint32_t j = jb_n;      if (!j) j = 1;
            uint32_t bytes_ps = (uint32_t)((int64_t)st_rx_bytes * 1000000 / elapsed);
            uint32_t good_ps  = (uint32_t)((int64_t)s_parser.n_good * 1000000 / elapsed);
            ESP_LOGI(TAG,
                     "bytes=%u/%d rxq=%u | good=%u crc=%u lost=%u | jb ms avg=%d min=%d max=%d | under=%u drop=%u plc=%u | dec us avg=%u max=%u | heap=%u",
                     (unsigned)bytes_ps, LINK_WIRE_BYTES_PER_SEC,
                     (unsigned)st_rxq_max,
                     (unsigned)good_ps, (unsigned)s_parser.n_crc_err,
                     (unsigned)s_parser.n_lost,
                     (int)(jb_sum / j * 1000 / LINK_SAMPLE_RATE),
                     (int)(jb_min == UINT32_MAX ? 0 : jb_min * 1000 / LINK_SAMPLE_RATE),
                     (int)(jb_max * 1000 / LINK_SAMPLE_RATE),
                     (unsigned)st_underruns, (unsigned)st_drops, (unsigned)st_concealed,
                     (unsigned)(st_dec_total_us / d), (unsigned)st_dec_max_us,
                     (unsigned)esp_get_free_heap_size());

            if (s_parser.n_good == 0 && st_rx_bytes > 0 && st_peek_len > 0) {
                char hex[3 * sizeof(st_peek) + 1];
                int  o = 0;
                for (uint32_t k = 0; k < st_peek_len; k++) {
                    o += snprintf(hex + o, sizeof(hex) - o, "%02X ", st_peek[k]);
                }
                ESP_LOGW(TAG, "bytes arriving but nothing syncs. raw: %s", hex);
                ESP_LOGW(TAG, "expected a frame to start A5 5A. if this looks "
                              "like noise the baud rates disagree; if you can see "
                              "A5 5A the payload length disagrees");
                st_peek_len = 0;
            }

            s_parser.n_good = 0; s_parser.n_crc_err = 0; s_parser.n_lost = 0;
            st_rx_bytes = 0; st_rxq_max = 0;
            st_underruns = 0; st_drops = 0; st_concealed = 0;
            st_dec_total_us = 0; st_dec_max_us = 0; st_decoded = 0;
            jb_sum = 0; jb_n = 0; jb_min = UINT32_MAX; jb_max = 0;
#endif
            t_report = now;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "LibreCom receiver starting");
    ESP_LOGI(TAG, "expecting %s, %d byte payload, %d frames/s, %d bytes/s at %d baud",
             LINK_SEND_PCM ? (LINK_PCM_ULAW ? "mu-law PCM" : "16-bit PCM") : "Codec 2",
             LINK_PAYLOAD_MAX, LINK_FRAMES_PER_SEC,
             LINK_WIRE_BYTES_PER_SEC, LINK_UART_BAUD);
    init_i2s();
#if !RX_LOCAL_TONE
    init_uart();
    /* Both audio tasks on core 1: the FPU state Codec 2 uses does not migrate
     * between Xtensa cores, and keeping them on one core makes the lock-free
     * ring between them safe by construction. */
    /* 32 KB: Opus takes its scratch from the stack (VAR_ARRAYS) and the
     * benchmark measured an 18 KB high-water mark at this configuration. */
    xTaskCreatePinnedToCore(net_task, "net", 32 * 1024, NULL, 6, NULL, 1);
#if RX_UART_LOOPBACK
    xTaskCreatePinnedToCore(loop_tx_task, "looptx", 4 * 1024, NULL, 5, NULL, 1);
#endif
#endif
    xTaskCreatePinnedToCore(dac_task, "dac", 4 * 1024, NULL, 7, NULL, 1);
}
