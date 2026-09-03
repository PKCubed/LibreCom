/*
 * LibreCom receiver: framed UART -> Codec 2 -> PCM5102A I2S DAC.
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

#include "codec2.h"
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

#define RING_SAMPLES     8192    /* power of two, ~1 s at 8 kHz */
#define JB_TARGET        (LINK_SAMPLE_RATE * JB_TARGET_MS / 1000)
#define JB_HIGH          (LINK_SAMPLE_RATE * JB_HIGH_MS   / 1000)

_Static_assert(LINK_PAYLOAD_LEN <= LINK_MAX_PAYLOAD, "payload too large for audio_link");
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
static uint8_t  st_peek[16];            /* first raw bytes, when nothing syncs */
static volatile uint32_t st_peek_len;
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

static void conceal(int frames)
{
    if (!s_have_last) {
        return;
    }
    static int16_t tmp[LINK_FRAME_SAMPLES];
    float gain = 0.6f;
    for (int f = 0; f < frames && f < PLC_MAX_FRAMES; f++) {
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            tmp[i] = (int16_t)((float)s_last_frame[i] * gain);
        }
        push_frame(tmp);
        st_concealed++;
        gain *= 0.6f;
    }
}

#if RX_UART_LOOPBACK
/* Feeds the internal loopback with frames this board builds itself, at the
 * real 20 ms cadence. In the PCM modes this is a genuine 440 Hz tone, so a
 * working loopback sounds exactly like stage 0. In Codec 2 mode the payload is
 * a fixed pattern and the audio is meaningless - watch good= and crc= instead. */
static void loop_tx_task(void *arg)
{
    (void)arg;
    static uint8_t payload[LINK_PAYLOAD_LEN];
    uint8_t frame[LINK_FRAME_LEN(LINK_PAYLOAD_LEN)];
    uint8_t seq = 0;
    TickType_t next = xTaskGetTickCount();
#if LINK_SEND_PCM
    static int16_t tone[LINK_FRAME_SAMPLES];
    float phase = 0.0f;
    const float inc = 2.0f * (float)M_PI * 440.0f / (float)LINK_SAMPLE_RATE;
#endif

    while (1) {
#if LINK_SEND_PCM
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            tone[i] = (int16_t)(12000.0f * sinf(phase));
            phase += inc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
#  if LINK_PCM_ULAW
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) payload[i] = link_ulaw_encode(tone[i]);
#  else
        memcpy(payload, tone, sizeof(payload));
#  endif
#else
        for (int i = 0; i < LINK_PAYLOAD_LEN; i++) payload[i] = (uint8_t)(0x5A + i);
#endif
        link_build_frame(frame, seq++, payload, LINK_PAYLOAD_LEN);
        uart_write_bytes(UART_PORT, (const char *)frame, sizeof(frame));
        vTaskDelayUntil(&next, pdMS_TO_TICKS(LINK_FRAME_MS));
    }
}
#endif /* RX_UART_LOOPBACK */

static void net_task(void *arg)
{
    (void)arg;

#if !LINK_SEND_PCM
    struct CODEC2 *c2 = codec2_create(CODEC2_MODE_3200);
    if (!c2) {
        ESP_LOGE(TAG, "codec2_create failed (out of heap?) - stopping");
        vTaskDelete(NULL);
        return;
    }
    int spf = codec2_samples_per_frame(c2);
    int bpf = (codec2_bits_per_frame(c2) + 7) / 8;
    ESP_LOGI(TAG, "Codec 2 mode 3200: %d samples/frame, %d bytes/frame", spf, bpf);
    if (spf != LINK_FRAME_SAMPLES || bpf != LINK_C2_BYTES) {
        ESP_LOGE(TAG, "link_config.h disagrees with Codec 2 (%d/%d) - stopping", spf, bpf);
        vTaskDelete(NULL);
        return;
    }
#endif

    link_parser_init(&s_parser, LINK_PAYLOAD_LEN);

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
            codec2_decode(c2, pcm, s_parser.payload);
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
    const float inc = 2.0f * (float)M_PI * 440.0f / (float)LINK_SAMPLE_RATE;
    ESP_LOGI(TAG, "RX_LOCAL_TONE: playing a local 440 Hz tone, UART ignored");
#else
    ESP_LOGI(TAG, "jitter buffer: target %d ms, drop above %d ms",
             JB_TARGET_MS, JB_HIGH_MS);
#endif

    int64_t  t_report = esp_timer_get_time();

    while (1) {
#if RX_LOCAL_TONE
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
             LINK_PAYLOAD_LEN, LINK_FRAMES_PER_SEC,
             LINK_WIRE_BYTES_PER_SEC, LINK_UART_BAUD);
    init_i2s();
#if !RX_LOCAL_TONE
    init_uart();
    /* Both audio tasks on core 1: the FPU state Codec 2 uses does not migrate
     * between Xtensa cores, and keeping them on one core makes the lock-free
     * ring between them safe by construction. */
    xTaskCreatePinnedToCore(net_task, "net", 24 * 1024, NULL, 6, NULL, 1);
#if RX_UART_LOOPBACK
    xTaskCreatePinnedToCore(loop_tx_task, "looptx", 4 * 1024, NULL, 5, NULL, 1);
#endif
#endif
    xTaskCreatePinnedToCore(dac_task, "dac", 4 * 1024, NULL, 7, NULL, 1);
}
