/*
 * AMR-NB feasibility and load benchmark.
 *
 * Same shape as the Opus benchmarks: measure before rewiring anything. Reports
 * per-mode encode/decode cost against the 20 ms frame budget, bytes on the
 * wire, heap and peak stack; then builds the multi-stream workloads to show how
 * many concurrent streams fit on one chip.
 *
 * AMR-NB is fixed 8 kHz / 160 samples / 20 ms. There is no sample rate or
 * complexity choice - only the bitrate mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "interf_enc.h"
#include "interf_dec.h"
#include "opus_bench.h"

static const char *TAG = "amrbench";

#define FRAME_MS      20
#define FS            8000
#define FRAME_SAMPLES (FS * FRAME_MS / 1000)      /* 160, fixed by the codec */
#define BUDGET_US     (FRAME_MS * 1000)
#define N_FRAMES      50
#define MAX_PKT       64
#define MAX_STREAMS   6

static int16_t s_audio[N_FRAMES * FRAME_SAMPLES];
static int16_t s_out[FRAME_SAMPLES];
static int16_t s_mix[FRAME_SAMPLES];
static uint8_t s_pkt[MAX_STREAMS][MAX_PKT];
static int     s_pkt_len[MAX_STREAMS];

static const struct { enum Mode mode; const char *name; int kbps_x100; } s_modes[] = {
    { MR475, "4.75", 475 }, { MR515, "5.15", 515 }, { MR59,  "5.90", 590 },
    { MR67,  "6.70", 670 }, { MR74,  "7.40", 740 }, { MR795, "7.95", 795 },
    { MR102, "10.2", 1020 }, { MR122, "12.2", 1220 },
};
#define N_MODES_TESTED ((int)(sizeof(s_modes) / sizeof(s_modes[0])))

/* Speech-like: harmonics of a moving fundamental plus noise, in float so the
 * S3 FPU handles it. Generated once, outside every timed region. */
static void generate_signal(void)
{
    float phase = 0.0f, t = 0.0f;
    for (int i = 0; i < N_FRAMES * FRAME_SAMPLES; i++) {
        float f0 = 130.0f * (1.0f + 0.15f * sinf(2.0f * (float)M_PI * 2.0f * t));
        int nharm = (int)((FS * 0.42f) / f0);
        if (nharm > 12) nharm = 12;
        float sum = 0.0f, norm = 0.0f;
        for (int k = 1; k <= nharm; k++) {
            float a = 1.0f / k;
            sum  += a * sinf(phase * k);
            norm += a;
        }
        s_audio[i] = (int16_t)(8000.0f * sum / norm
                               + 300.0f * (2.0f * (float)rand() / (float)RAND_MAX - 1.0f));
        phase += 2.0f * (float)M_PI * f0 / FS;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        t += 1.0f / FS;
    }
}

static void bench_mode(int idx)
{
    size_t heap_before = esp_get_free_heap_size();

    void *enc = Encoder_Interface_init(0);        /* 0 = DTX off */
    if (!enc) { ESP_LOGE(TAG, "encoder init failed"); return; }
    void *dec = Decoder_Interface_init();
    if (!dec) { ESP_LOGE(TAG, "decoder init failed"); Encoder_Interface_exit(enc); return; }

    size_t heap_after = esp_get_free_heap_size();

    int64_t enc_total = 0, enc_max = 0, dec_total = 0, dec_max = 0;
    int bytes_total = 0, bad = 0;
    uint8_t pkt[MAX_PKT];

    for (int f = 0; f < N_FRAMES; f++) {
        const int16_t *pcm = &s_audio[f * FRAME_SAMPLES];

        int64_t t0 = esp_timer_get_time();
        int n = Encoder_Interface_Encode(enc, s_modes[idx].mode, pcm, pkt, 0);
        int64_t dt = esp_timer_get_time() - t0;
        if (n <= 0 || n > MAX_PKT) { bad++; continue; }
        enc_total += dt;
        if (dt > enc_max) enc_max = dt;
        bytes_total += n;

        t0 = esp_timer_get_time();
        Decoder_Interface_Decode(dec, pkt, s_out, 0);
        dt = esp_timer_get_time() - t0;
        dec_total += dt;
        if (dt > dec_max) dec_max = dt;

        if ((f % 8) == 7) vTaskDelay(1);
    }

    int enc_avg = (int)(enc_total / N_FRAMES);
    int dec_avg = (int)(dec_total / N_FRAMES);
    int bytes_avg = bytes_total / N_FRAMES;

    ESP_LOGI(TAG,
             "MR%-5s %5s kbps | enc %5d us (%2d%%) max %5d | dec %5d us (%2d%%) max %5d | %2d B/frame = %2d kbps wire | state %u B | stack %u B | bad %d",
             s_modes[idx].name, s_modes[idx].name,
             enc_avg, 100 * enc_avg / BUDGET_US, (int)enc_max,
             dec_avg, 100 * dec_avg / BUDGET_US, (int)dec_max,
             bytes_avg, (bytes_avg + 6) * 8 * (1000 / FRAME_MS) / 1000,
             (unsigned)(heap_before - heap_after),
             (unsigned)(24 * 1024 - uxTaskGetStackHighWaterMark(NULL)), bad);

    Encoder_Interface_exit(enc);
    Decoder_Interface_exit(dec);
}

/* One optional encoder plus n_dec decoders, all timed together against a single
 * frame period, with mixing included because that is real work. */
static void bench_load(const char *label, int with_encoder, int n_dec, enum Mode mode)
{
    void *enc = NULL;
    void *dec[MAX_STREAMS] = { 0 };

    size_t heap_before = esp_get_free_heap_size();

    if (with_encoder) {
        enc = Encoder_Interface_init(0);
        if (!enc) { ESP_LOGE(TAG, "%s: encoder init failed", label); return; }
    }
    /* Build one representative packet per stream, outside the timed loop. */
    void *seed = Encoder_Interface_init(0);
    for (int i = 0; i < n_dec; i++) {
        dec[i] = Decoder_Interface_init();
        if (!dec[i]) {
            ESP_LOGE(TAG, "%s: decoder %d init failed - out of heap", label, i);
            goto cleanup;
        }
        s_pkt_len[i] = Encoder_Interface_Encode(seed, mode, &s_audio[i * FRAME_SAMPLES],
                                                s_pkt[i], 0);
    }
    Encoder_Interface_exit(seed);
    seed = NULL;

    size_t heap_after = esp_get_free_heap_size();

    int64_t total = 0, worst = 0;
    for (int it = 0; it < N_FRAMES; it++) {
        const int16_t *pcm = &s_audio[(it % N_FRAMES) * FRAME_SAMPLES];
        uint8_t tmp[MAX_PKT];

        int64_t t0 = esp_timer_get_time();
        if (enc) {
            Encoder_Interface_Encode(enc, mode, pcm, tmp, 0);
        }
        for (int i = 0; i < n_dec; i++) {
            Decoder_Interface_Decode(dec[i], s_pkt[i], s_out, 0);
            for (int k = 0; k < FRAME_SAMPLES; k++) {
                int32_t v = s_mix[k] + s_out[k];
                s_mix[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }
        int64_t dt = esp_timer_get_time() - t0;
        total += dt;
        if (dt > worst) worst = dt;

        if ((it % 8) == 7) vTaskDelay(1);
    }

    int avg = (int)(total / N_FRAMES);
    ESP_LOGI(TAG, "%-30s %6d us avg (%3d%%)  worst %6d us (%3d%%)  state %u KB",
             label, avg, 100 * avg / BUDGET_US,
             (int)worst, (int)(100 * worst / BUDGET_US),
             (unsigned)((heap_before - heap_after) / 1024));

cleanup:
    if (seed) Encoder_Interface_exit(seed);
    if (enc) Encoder_Interface_exit(enc);
    for (int i = 0; i < n_dec; i++) if (dec[i]) Decoder_Interface_exit(dec[i]);
}

static void amr_task(void *arg)
{
    (void)arg;
    srand(2468);
    generate_signal();

    ESP_LOGI(TAG, "AMR-NB (opencore-amr), fixed 8 kHz / 160 samples / 20 ms");
    ESP_LOGI(TAG, "budget is %d us per frame; enc and dec run on separate boards",
             BUDGET_US);
    ESP_LOGI(TAG, "free heap %u B, largest block %u B",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    ESP_LOGI(TAG, "--- all eight bitrate modes ---");
    for (int i = 0; i < N_MODES_TESTED; i++) {
        bench_mode(i);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "--- multi-stream at MR122 (12.2 kbps, best quality) ---");
    bench_load("1 decode",            0, 1, MR122);
    bench_load("4 decode",            0, 4, MR122);
    bench_load("1 encode",            1, 0, MR122);
    bench_load("1 encode + 3 decode", 1, 3, MR122);
    bench_load("1 encode + 5 decode", 1, 5, MR122);

    ESP_LOGI(TAG, "peak stack for this task: %u B of %d B",
             (unsigned)(24 * 1024 - uxTaskGetStackHighWaterMark(NULL)), 24 * 1024);
    ESP_LOGI(TAG, "free heap after: %u B", (unsigned)esp_get_free_heap_size());
    ESP_LOGW(TAG, "AMR-NB benchmark done - set TX_BENCH to 0 for normal operation");
    vTaskDelete(NULL);
}

void amr_bench_run(void)
{
    /* AMR-NB is old DSP-style code that keeps its state on the heap, so it
     * should want far less stack than Opus. 24 KB is generous; the high water
     * mark tells us what the real audio task needs. */
    xTaskCreatePinnedToCore(amr_task, "amrbench", 24 * 1024, NULL, 6, NULL, 1);
}
