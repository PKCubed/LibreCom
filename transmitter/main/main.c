/*
 * LibreCom transmitter: PCM1808 I2S ADC -> Opus -> framed UART.
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

#include "opus.h"
#include "audio_link.h"
#include "link_config.h"
#include "opus_bench.h"

/* ------------------------------------------------------------------ *
 *  Bring-up switches
 * ------------------------------------------------------------------ */

/* Where the audio comes from.
 *   TX_SRC_ADC   - the real PCM1808.
 *   TX_SRC_SINE  - a 440 Hz tone generated on-chip. I2S is not even started,
 *                  so this tests {codec + UART + receiver} with the ADC
 *                  completely out of the picture. */
#define TX_SRC_ADC       0
#define TX_SRC_SINE      1
#define TX_SOURCE        TX_SRC_ADC   /* TX_SRC_SINE for the internal tone */

/* 1 = run the Opus timing benchmark at boot instead of the audio path.
 * Worth re-running after any change to bitrate, complexity or frame size. */
#define TX_OPUS_BENCH    0

/* Run the I2S bus at LINK_SAMPLE_RATE * ADC_DECIM and filter back down in
 * software. At 16 kHz the PCM1808 sits comfortably inside its range, so 1 is
 * normally right; 2 or 4 are there if the ADC ever misbehaves. */
#define ADC_DECIM        1

/* MCLK fed to the PCM1808 SCKI pin, as a multiple of the sample rate. At
 * 16 kHz, 256x gives 4.096 MHz - twice the part's documented minimum, and a
 * good deal healthier than the 2.048 MHz it saw at 8 kHz. */
#define ADC_MCLK_MULT    I2S_MCLK_MULTIPLE_256

#define ADC_USE_RIGHT    0        /* 0 = left I2S slot, 1 = right slot */
#define ADC_GAIN         1.0f     /* raise if the level report is small */
#define ADC_DC_BLOCK     1

/* 1 = report where the captured audio actually has energy, once a second.
 * Codec 2 at 8 kHz, mu-law at 8 kHz and Opus at 16 kHz all sounded equally
 * muffled, which says the band limit is upstream of the codec. This measures
 * that directly instead of inferring it. */
#define TX_SPECTRUM      1

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
#define BLOCK_IN      (LINK_FRAME_SAMPLES * ADC_DECIM)   /* input samples per frame */

_Static_assert(LINK_PAYLOAD_MAX <= LINK_MAX_PAYLOAD, "payload too large for audio_link");
_Static_assert(ADC_DECIM >= 1 && ADC_DECIM <= 8, "unsupported ADC_DECIM");

/* The decimating filter only exists when we are actually reading the ADC. */
#if (TX_SOURCE == TX_SRC_ADC) && (ADC_DECIM > 1)
#  define USE_DECIM 1
#else
#  define USE_DECIM 0
#endif

/* Working buffers. Static rather than on the task stack, because Opus wants
 * most of the stack it is given for its own scratch. */
#if TX_SOURCE == TX_SRC_ADC
static i2s_chan_handle_t s_rx_chan;
static int32_t s_i2s_raw[BLOCK_IN * 2];     /* stereo 32-bit slots off the bus */
static float   s_fin[BLOCK_IN];             /* one channel, still at I2S_RATE */
static float   s_f16[LINK_FRAME_SAMPLES];   /* at LINK_SAMPLE_RATE */
#endif
static int16_t s_pcm[LINK_FRAME_SAMPLES];   /* what the encoder sees */

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
    /* Just under the output Nyquist. */
    const float fc = (LINK_SAMPLE_RATE * 0.45f) / (float)I2S_RATE;
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
    /* One-pole high pass, about 3 Hz at 16 kHz. */
    float y  = x - s_dc_x1 + 0.999f * s_dc_y1;
    s_dc_x1  = x;
    s_dc_y1  = y;
    return y;
}
#endif

/* ------------------------------------------------------------------ *
 *  Band energy meter
 *
 *  A Goertzel resonator per frequency: cheaper than an FFT when only a
 *  handful of points are wanted, and enough to see where the energy stops.
 *  Levels are printed in dB relative to the loudest band, so the absolute
 *  input level does not matter - only the shape does.
 * ------------------------------------------------------------------ */
#if TX_SPECTRUM
static const int s_bands[] = { 250, 500, 1000, 2000, 3000, 4000, 5000, 6500 };
#define N_BANDS  ((int)(sizeof(s_bands) / sizeof(s_bands[0])))
static float s_band_pow[N_BANDS];
static float s_band_coeff[N_BANDS];

static void spectrum_init(void)
{
    for (int b = 0; b < N_BANDS; b++) {
        float w = 2.0f * (float)M_PI * (float)s_bands[b] / (float)LINK_SAMPLE_RATE;
        s_band_coeff[b] = 2.0f * cosf(w);
    }
}

static void spectrum_accumulate(const int16_t *x, int n)
{
    for (int b = 0; b < N_BANDS; b++) {
        const float c = s_band_coeff[b];
        float s1 = 0.0f, s2 = 0.0f;
        for (int i = 0; i < n; i++) {
            float s0 = (float)x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        s_band_pow[b] += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
}

static void spectrum_report(void)
{
    float peak = 0.0f;
    for (int b = 0; b < N_BANDS; b++) {
        if (s_band_pow[b] > peak) peak = s_band_pow[b];
    }
    char line[160];
    int o = 0;
    for (int b = 0; b < N_BANDS; b++) {
        int db = (peak > 0.0f && s_band_pow[b] > 0.0f)
                 ? (int)(10.0f * log10f(s_band_pow[b] / peak)) : -99;
        if (db < -99) db = -99;
        o += snprintf(line + o, sizeof(line) - o, "%d:%ddB ", s_bands[b], db);
        s_band_pow[b] = 0.0f;
    }
    ESP_LOGI(TAG, "input spectrum (dB below loudest) %s", line);
}
#endif /* TX_SPECTRUM */

/* ------------------------------------------------------------------ *
 *  Peripherals
 * ------------------------------------------------------------------ */
#if TX_SOURCE == TX_SRC_ADC
static void init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    /* One DMA descriptor caps out at 4092 bytes and 32-bit stereo costs 8
     * bytes per frame, so keep the descriptor small and scale the count. */
    chan_cfg.dma_frame_num = LINK_FRAME_SAMPLES;
    chan_cfg.dma_desc_num  = 4 * ADC_DECIM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    /* 32-bit slots put BCK at 64 fs, which is what the PCM1808 expects in
     * slave mode. The 24-bit sample lands in bits [31:8], so >> 16 leaves a
     * clean 16-bit sample. */
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

    for (int i = 0; i < BLOCK_IN; i++) {
        s_fin[i] = (float)(s_i2s_raw[i * 2 + ADC_USE_RIGHT] >> 16);
    }

#if USE_DECIM
    decimate(s_fin, s_f16);
#else
    memcpy(s_f16, s_fin, sizeof(s_f16));
#endif

    for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
        float v = s_f16[i] * ADC_GAIN;
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

    static uint8_t frame[LINK_FRAME_LEN(LINK_PAYLOAD_MAX)];
    static uint8_t payload[LINK_PAYLOAD_MAX];
    uint8_t seq = 0;

#if !LINK_SEND_PCM
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(LINK_SAMPLE_RATE, 1,
                                           OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %s - stopping", opus_strerror(err));
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(LINK_OPUS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(LINK_OPUS_COMPLEXITY));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));            /* CBR: steady wire rate */
    opus_encoder_ctl(enc, OPUS_SET_DTX(0));
#if LINK_OPUS_FORCE_BW
    opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(LINK_OPUS_FORCE_BW));
    ESP_LOGW(TAG, "bandwidth forced by LINK_OPUS_FORCE_BW, not chosen by Opus");
#endif
    ESP_LOGI(TAG, "Opus %s: %d Hz, %d bps, complexity %d, %d ms frames",
             opus_get_version_string(), LINK_SAMPLE_RATE, LINK_OPUS_BITRATE,
             LINK_OPUS_COMPLEXITY, LINK_FRAME_MS);
#endif

    /* Per-second statistics */
    uint32_t n_frames = 0;
    int32_t  peak = 0;
    int64_t  sum_sq = 0, sum = 0;
    int64_t  enc_total = 0, enc_max = 0;
    int64_t  wait_total = 0;
    uint32_t wr_total = 0, wr_err = 0, enc_bytes = 0, enc_fail = 0;
    int64_t  t_report = esp_timer_get_time();

#if TX_SOURCE == TX_SRC_SINE
    TickType_t next_wake = xTaskGetTickCount();
    ESP_LOGI(TAG, "source: internal %d Hz tone", (int)TONE_HZ);
#else
    ESP_LOGI(TAG, "source: PCM1808, decim %d, gain %.2f", ADC_DECIM, ADC_GAIN);
#endif
    ESP_LOGI(TAG, "sending %s, %d frames/s, about %d bytes/s at %d baud",
             LINK_SEND_PCM ? (LINK_PCM_ULAW ? "mu-law PCM" : "16-bit PCM") : "Opus",
             LINK_FRAMES_PER_SEC, LINK_WIRE_BYTES_PER_SEC, LINK_UART_BAUD);

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

#if TX_SPECTRUM
        spectrum_accumulate(s_pcm, LINK_FRAME_SAMPLES);
#endif

        int payload_len;
#if LINK_SEND_PCM
#  if LINK_PCM_ULAW
        for (int i = 0; i < LINK_FRAME_SAMPLES; i++) {
            payload[i] = link_ulaw_encode(s_pcm[i]);
        }
        payload_len = LINK_FRAME_SAMPLES;
#  else
        memcpy(payload, s_pcm, LINK_FRAME_SAMPLES * sizeof(int16_t));
        payload_len = LINK_FRAME_SAMPLES * 2;
#  endif
#else
        int64_t t0 = esp_timer_get_time();
        payload_len = opus_encode(enc, s_pcm, LINK_FRAME_SAMPLES,
                                  payload, LINK_PAYLOAD_MAX);
        int64_t dt = esp_timer_get_time() - t0;
        enc_total += dt;
        if (dt > enc_max) enc_max = dt;

        if (payload_len < 0) {
            ESP_LOGW(TAG, "opus_encode: %s", opus_strerror(payload_len));
            enc_fail++;
            continue;
        }
        enc_bytes += (uint32_t)payload_len;
#endif

        size_t flen = link_build_frame(frame, seq++, payload, (size_t)payload_len);
        int wrote = uart_write_bytes(UART_PORT, (const char *)frame, flen);
        if (wrote == (int)flen) {
            wr_total += (uint32_t)wrote;
        } else {
            wr_err++;
            if (wrote > 0) wr_total += (uint32_t)wrote;
        }
        n_frames++;

        int64_t now = esp_timer_get_time();
        if (now - t_report >= 1000000) {
            int64_t  elapsed = now - t_report;
            uint32_t n  = n_frames ? n_frames : 1;
            uint32_t ns = n * LINK_FRAME_SAMPLES;
            uint32_t fps     = (uint32_t)((int64_t)n_frames * 1000000 / elapsed);
            uint32_t wr_ps   = (uint32_t)((int64_t)wr_total * 1000000 / elapsed);
            uint32_t wait_ms = (uint32_t)(wait_total * 1000 / elapsed);
            int rms = (int)sqrtf((float)(sum_sq / (int64_t)ns));
            int dc  = (int)(sum / (int64_t)ns);

            ESP_LOGI(TAG,
                     "frames=%u peak=%d rms=%d dc=%d | wr=%u/%d werr=%u | enc us avg=%d max=%d (%d%%) | %d B/frame | i2s wait ms=%d | heap=%u",
                     (unsigned)fps, (int)peak, rms, dc,
                     (unsigned)wr_ps, LINK_WIRE_BYTES_PER_SEC, (unsigned)wr_err,
                     (int)(enc_total / n), (int)enc_max,
                     (int)(enc_total / n * 100 / (LINK_FRAME_MS * 1000)),
                     (int)(enc_bytes / n),
                     (int)wait_ms,
                     (unsigned)esp_get_free_heap_size());
            if (enc_fail) {
                ESP_LOGW(TAG, "%u encode failures this second", (unsigned)enc_fail);
            }
#if TX_SPECTRUM
            spectrum_report();
#endif
#if !LINK_SEND_PCM
            {
                /* Opus picks its own bandwidth from the bitrate unless told
                 * otherwise. If it has quietly settled on narrowband, feeding
                 * it 16 kHz buys nothing. */
                opus_int32 bw = 0;
                opus_encoder_ctl(enc, OPUS_GET_BANDWIDTH(&bw));
                const char *bws =
                    bw == OPUS_BANDWIDTH_NARROWBAND    ? "narrowband 4 kHz"  :
                    bw == OPUS_BANDWIDTH_MEDIUMBAND    ? "mediumband 6 kHz"  :
                    bw == OPUS_BANDWIDTH_WIDEBAND      ? "wideband 8 kHz"    :
                    bw == OPUS_BANDWIDTH_SUPERWIDEBAND ? "superwide 12 kHz"  :
                    bw == OPUS_BANDWIDTH_FULLBAND      ? "fullband 20 kHz"   : "?";
                if (bw < OPUS_BANDWIDTH_WIDEBAND) {
                    ESP_LOGW(TAG, "opus dropped to %s - raise LINK_OPUS_BITRATE "
                                  "or it will sound muffled", bws);
                } else {
                    ESP_LOGI(TAG, "opus is encoding at %s", bws);
                }
            }
#endif

            n_frames = 0; peak = 0; sum = 0; sum_sq = 0;
            enc_total = 0; enc_max = 0; wait_total = 0;
            wr_total = 0; wr_err = 0; enc_bytes = 0; enc_fail = 0;
            t_report = now;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "LibreCom transmitter starting");

#if TX_OPUS_BENCH
    /* Run the benchmark on its own, with nothing else on the audio core. The
     * normal task would preempt it every frame and inflate every timing we are
     * trying to measure, so the audio path stays down until this is set to 0. */
    ESP_LOGW(TAG, "TX_OPUS_BENCH is on: audio path disabled while measuring");
    opus_bench_run();
    return;
#endif

    init_uart();
#if TX_SOURCE == TX_SRC_ADC
    init_i2s();
#endif
#if TX_SPECTRUM
    spectrum_init();
#endif
#if USE_DECIM
    fir_design();
    ESP_LOGI(TAG, "decimating %d -> %d Hz with a %d-tap FIR",
             I2S_RATE, LINK_SAMPLE_RATE, FIR_TAPS);
#endif

    /* Pinned to core 1, away from console and system housekeeping. 32 KB
     * because Opus takes its scratch from the stack (VAR_ARRAYS): the
     * benchmark measured an 18 KB high-water mark at this configuration. */
    xTaskCreatePinnedToCore(tx_task, "tx", 32 * 1024, NULL, 6, NULL, 1);
}
