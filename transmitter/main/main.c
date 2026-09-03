/*
 * LibreCom transmitter: PCM1808 I2S ADC -> Codec 2 -> framed UART.
 *
 * Bring-up switches live in the block below; the settings that have to match
 * the receiver live in components/audio_link/include/link_config.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

/* Where the audio comes from.
 *   TX_SRC_ADC   - the real PCM1808.
 *   TX_SRC_SINE  - a 440 Hz tone generated on-chip. I2S is not even started,
 *                  so this tests {Codec 2 + UART + receiver} with the ADC
 *                  completely out of the picture. */
#define TX_SRC_ADC       0
#define TX_SRC_SINE      1
#define TX_SOURCE        TX_SRC_ADC   /* TX_SRC_SINE for the internal tone */

/* Run the I2S bus at LINK_SAMPLE_RATE * ADC_DECIM and filter back down to
 * 8 kHz in software. Use 1 first; raise it to 2/4/6 if the PCM1808 will not
 * run at 8 kHz (its system clock spec bottoms out right at 8 kHz x 256). */
#define ADC_DECIM        1

/* MCLK the ESP32 feeds to the PCM1808 SCKI pin, as a multiple of the sample
 * rate. 256 gives 2.048 MHz at 8 kHz, which is the documented minimum for
 * that part. 512 gives 4.096 MHz and more margin, if the board is happy. */
#define ADC_MCLK_MULT    I2S_MCLK_MULTIPLE_256

#define ADC_USE_RIGHT    0        /* 0 = left I2S slot, 1 = right slot */
#define ADC_GAIN         1.0f     /* raise if the level report is small */
#define ADC_DC_BLOCK     1

/* Pins */
#define I2S_MCK_IO       (16)     /* -> PCM1808 SCKI */
#define I2S_BCK_IO       (17)     /* -> PCM1808 BCK  */
#define I2S_WS_IO        (18)     /* -> PCM1808 LRCK */
#define I2S_DIN_IO       (19)     /* <- PCM1808 DOUT */

#define UART_PORT        (UART_NUM_1)
#if LINK_SWAP_PINS
#  define UART_TX_IO     (5)      /* -> receiver GPIO4, the other jumper */
#  define UART_RX_IO     (4)
#else
#  define UART_TX_IO     (4)      /* -> receiver GPIO5 */
#  define UART_RX_IO     (5)      /* unused here, but the driver wants a pin */
#endif

/* ------------------------------------------------------------------ */

static const char *TAG = "tx";

#define I2S_RATE      (LINK_SAMPLE_RATE * ADC_DECIM)
#define BLOCK_IN      (LINK_FRAME_SAMPLES * ADC_DECIM)   /* input samples per 20 ms */

_Static_assert(LINK_PAYLOAD_LEN <= LINK_MAX_PAYLOAD, "payload too large for audio_link");
_Static_assert(ADC_DECIM >= 1 && ADC_DECIM <= 8, "unsupported ADC_DECIM");

/* The decimating filter only exists when we are actually reading the ADC. */
#if (TX_SOURCE == TX_SRC_ADC) && (ADC_DECIM > 1)
#  define USE_DECIM 1
#else
#  define USE_DECIM 0
#endif

/* Working buffers. Static rather than on the task stack, because Codec 2
 * already wants most of the stack it is given. */
#if TX_SOURCE == TX_SRC_ADC
static i2s_chan_handle_t s_rx_chan;
static int32_t s_i2s_raw[BLOCK_IN * 2];     /* stereo 32-bit slots off the bus */
static float   s_fin[BLOCK_IN];             /* one channel, still at I2S_RATE */
static float   s_f8[LINK_FRAME_SAMPLES];    /* at 8 kHz */
#endif
static int16_t s_pcm[LINK_FRAME_SAMPLES];   /* what Codec 2 sees */

/* ------------------------------------------------------------------ *
 *  Decimating low-pass (only built when ADC_DECIM > 1)
 * ------------------------------------------------------------------ */
#if USE_DECIM
#define FIR_TAPS   (16 * ADC_DECIM + 1)
#define FIR_HIST   (FIR_TAPS - 1)
static float s_fir[FIR_TAPS];
static float s_hist[FIR_HIST + BLOCK_IN];

static void fir_design(void)
{
    const float fc = 3400.0f / (float)I2S_RATE;   /* normalised cutoff */
    const int   M  = FIR_TAPS - 1;
    float sum = 0.0f;

    for (int n = 0; n <= M; n++) {
        float k    = (float)n - (float)M * 0.5f;
        float sinc = (fabsf(k) < 1e-6f)
                     ? (2.0f * fc)
                     : sinf(2.0f * (float)M_PI * fc * k) / ((float)M_PI * k);
        float w    = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)n / (float)M);
        s_fir[n]   = sinc * w;
        sum       += s_fir[n];
    }
    for (int n = 0; n <= M; n++) {
        s_fir[n] /= sum;                          /* unity gain at DC */
    }
}

/* Filters BLOCK_IN input samples down to LINK_FRAME_SAMPLES output samples.
 * The taps are only evaluated at output instants, so this costs 1/ADC_DECIM
 * of a full-rate FIR. */
static void decimate(const float *in, float *out)
{
    memcpy(&s_hist[FIR_HIST], in, sizeof(float) * BLOCK_IN);
    for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
        const float *x = &s_hist[i * ADC_DECIM];
        float acc = 0.0f;
        for (int t = 0; t < FIR_TAPS; t++) {
            acc += s_fir[t] * x[t];
        }
        out[i] = acc;
    }
    memmove(s_hist, &s_hist[BLOCK_IN], sizeof(float) * FIR_HIST);
}
#endif /* USE_DECIM */

/* ------------------------------------------------------------------ */

static inline int16_t clamp16(float v)
{
    if (v >  32767.0f) return  32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

#if ADC_DC_BLOCK && (TX_SOURCE == TX_SRC_ADC)
static float s_dc_x1, s_dc_y1;
static inline float dc_block(float x)
{
    float y  = x - s_dc_x1 + 0.995f * s_dc_y1;
    s_dc_x1  = x;
    s_dc_y1  = y;
    return y;
}
#endif

/* ------------------------------------------------------------------ *
 *  Peripherals
 * ------------------------------------------------------------------ */
#if TX_SOURCE == TX_SRC_ADC
static void init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    /* One DMA descriptor caps out at 4092 bytes, and 32-bit stereo costs
     * 8 bytes per frame. Keeping the descriptor at 160 frames (1280 bytes)
     * stays well inside that at any ADC_DECIM, so scale the descriptor count
     * instead to hold a steady ~80 ms of audio. */
    chan_cfg.dma_frame_num = LINK_FRAME_SAMPLES;
    chan_cfg.dma_desc_num  = 4 * ADC_DECIM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    /* 32-bit slots on purpose: that puts BCK at 64 fs, which is what the
     * PCM1808 expects in slave mode. 16-bit slots give 32 fs and the part is
     * not specified to work there. The 24-bit sample lands in bits [31:8] of
     * each word, so a >> 16 leaves a clean 16-bit sample. */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_IO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = ADC_MCLK_MULT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    ESP_LOGI(TAG, "I2S RX up: %d Hz, 32-bit slots, BCK %d Hz, MCLK %d Hz",
             I2S_RATE, I2S_RATE * 64, I2S_RATE * (int)ADC_MCLK_MULT);
}
#endif

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
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_IO, UART_RX_IO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d up at %d baud, TX on GPIO%d",
             (int)UART_PORT, LINK_UART_BAUD, UART_TX_IO);
}

/* ------------------------------------------------------------------ *
 *  Audio capture
 * ------------------------------------------------------------------ */
#if TX_SOURCE == TX_SRC_SINE
#define TONE_HZ     440.0f
#define TONE_AMPL   12000.0f
static float s_phase;

static void source_fill(int16_t *out)
{
    const float inc = 2.0f * (float)M_PI * TONE_HZ / (float)LINK_SAMPLE_RATE;
    for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
        out[i] = (int16_t)(TONE_AMPL * sinf(s_phase));
        s_phase += inc;
        if (s_phase >= 2.0f * (float)M_PI) {
            s_phase -= 2.0f * (float)M_PI;
        }
    }
}
#else
/* Returns the microseconds spent blocked in i2s_channel_read(), which reads
 * out directly as how much slack the encoder has left. */
static int64_t source_fill(int16_t *out)
{
    size_t got = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = i2s_channel_read(s_rx_chan, s_i2s_raw, sizeof(s_i2s_raw),
                                     &got, pdMS_TO_TICKS(200));
    int64_t wait = esp_timer_get_time() - t0;

    if (err != ESP_OK || got != sizeof(s_i2s_raw)) {
        ESP_LOGW(TAG, "i2s read: %s, %u/%u bytes", esp_err_to_name(err),
                 (unsigned)got, (unsigned)sizeof(s_i2s_raw));
        memset(out, 0, sizeof(int16_t) * LINK_FRAME_SAMPLES);
        return wait;
    }

    /* Pick one slot and drop the 24-bit word down to 16 bits. */
    for (int i = 0; i < BLOCK_IN; i++) {
        s_fin[i] = (float)(s_i2s_raw[i * 2 + ADC_USE_RIGHT] >> 16);
    }

#if USE_DECIM
    decimate(s_fin, s_f8);
#else
    memcpy(s_f8, s_fin, sizeof(s_f8));
#endif

    for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
        float v = s_f8[i] * ADC_GAIN;
#if ADC_DC_BLOCK
        v = dc_block(v);
#endif
        out[i] = clamp16(v);
    }
    return wait;
}
#endif

/* ------------------------------------------------------------------ *
 *  Main loop
 * ------------------------------------------------------------------ */
static void tx_task(void *arg)
{
    (void)arg;

    uint8_t frame[LINK_FRAME_LEN(LINK_PAYLOAD_LEN)];
    uint8_t seq = 0;
#if LINK_SEND_PCM && LINK_PCM_ULAW
    uint8_t payload[LINK_PAYLOAD_LEN];
#endif

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
    unsigned char bits[LINK_C2_BYTES];
#endif

    /* Per-second statistics */
    uint32_t n_frames = 0;
    int32_t  peak = 0;
    int64_t  sum_sq = 0, sum = 0;
    int64_t  enc_total = 0, enc_max = 0;
    int64_t  wait_total = 0;
    uint32_t wr_total = 0, wr_err = 0;
    int64_t  t_report = esp_timer_get_time();

#if TX_SOURCE == TX_SRC_SINE
    TickType_t next_wake = xTaskGetTickCount();
    ESP_LOGI(TAG, "source: internal %d Hz tone", (int)TONE_HZ);
#else
    ESP_LOGI(TAG, "source: PCM1808, decim %d, gain %.2f", ADC_DECIM, ADC_GAIN);
#endif
    ESP_LOGI(TAG, "sending %s, %d byte payload, %d frames/s, %d bytes/s at %d baud",
             LINK_SEND_PCM ? (LINK_PCM_ULAW ? "mu-law PCM" : "16-bit PCM") : "Codec 2",
             LINK_PAYLOAD_LEN, LINK_FRAMES_PER_SEC,
             LINK_WIRE_BYTES_PER_SEC, LINK_UART_BAUD);

    while (1) {
#if TX_SOURCE == TX_SRC_SINE
        source_fill(s_pcm);
        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(LINK_FRAME_MS));
#else
        wait_total += source_fill(s_pcm);
#endif

        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            int32_t v = s_pcm[i];
            int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sum    += v;
            sum_sq += (int64_t)v * v;
        }

#if LINK_SEND_PCM
#  if LINK_PCM_ULAW
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            payload[i] = link_ulaw_encode(s_pcm[i]);
        }
        link_build_frame(frame, seq++, payload, LINK_PAYLOAD_LEN);
#  else
        link_build_frame(frame, seq++, (const uint8_t *)s_pcm, LINK_PAYLOAD_LEN);
#  endif
#else
        int64_t t0 = esp_timer_get_time();
        codec2_encode(c2, bits, s_pcm);
        int64_t dt = esp_timer_get_time() - t0;
        enc_total += dt;
        if (dt > enc_max) enc_max = dt;

        link_build_frame(frame, seq++, bits, LINK_PAYLOAD_LEN);
#endif
        /* Check the driver actually took the bytes. If wr= reads 8200 and the
         * receiver still sees bytes=0, the data reached the UART driver and
         * the problem is past this chip: the pin, the jumper, or the far end. */
        int wrote = uart_write_bytes(UART_PORT, (const char *)frame, sizeof(frame));
        if (wrote == (int)sizeof(frame)) {
            wr_total += (uint32_t)wrote;
        } else {
            wr_err++;
            if (wrote > 0) {
                wr_total += (uint32_t)wrote;
            }
        }
        n_frames++;

        int64_t now = esp_timer_get_time();
        if (now - t_report >= 1000000) {
            /* The window is a whole number of frames, so it overshoots 1 s by
             * up to one frame time. Scale the counts to a true per-second rate
             * so a healthy link reads exactly 50 and 600 every time. */
            int64_t  elapsed = now - t_report;
            uint32_t n  = n_frames ? n_frames : 1;
            uint32_t ns = n * LINK_FRAME_SAMPLES;
            uint32_t fps    = (uint32_t)((int64_t)n_frames * 1000000 / elapsed);
            uint32_t wr_ps  = (uint32_t)((int64_t)wr_total * 1000000 / elapsed);
            uint32_t wait_ms = (uint32_t)(wait_total * 1000 / elapsed);
            int rms = (int)sqrtf((float)(sum_sq / (int64_t)ns));
            int dc  = (int)(sum / (int64_t)ns);

            ESP_LOGI(TAG,
                     "frames=%u peak=%d rms=%d dc=%d | wr=%u/%d werr=%u | enc us avg=%d max=%d | i2s wait ms=%d | heap=%u",
                     (unsigned)fps, (int)peak, rms, dc,
                     (unsigned)wr_ps, LINK_WIRE_BYTES_PER_SEC, (unsigned)wr_err,
                     (int)(enc_total / n), (int)enc_max,
                     (int)wait_ms,
                     (unsigned)esp_get_free_heap_size());

            n_frames = 0; peak = 0; sum = 0; sum_sq = 0;
            enc_total = 0; enc_max = 0; wait_total = 0;
            wr_total = 0; wr_err = 0;
            t_report = now;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "LibreCom transmitter starting");
    init_uart();
#if TX_SOURCE == TX_SRC_ADC
    init_i2s();
#endif
#if USE_DECIM
    fir_design();
    ESP_LOGI(TAG, "decimating %d -> %d Hz with a %d-tap FIR",
             I2S_RATE, LINK_SAMPLE_RATE, FIR_TAPS);
#endif

    /* Pinned to core 1: Codec 2 leans on the FPU and Xtensa coprocessor state
     * does not migrate between cores. It also keeps the audio loop clear of
     * the core running console and system housekeeping. */
    xTaskCreatePinnedToCore(tx_task, "tx", 24 * 1024, NULL, 6, NULL, 1);
}
