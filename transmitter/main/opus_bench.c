/*
 * Opus feasibility benchmark.
 *
 * Answers the only question that matters before rewiring the audio path: can
 * this chip encode (and decode) a 20 ms frame well inside 20 ms, and does the
 * codec state fit in RAM?
 *
 * Runs a matrix of sample rates and complexity settings against a synthetic
 * voiced signal, and reports microseconds per frame, bytes produced, heap
 * consumed and peak stack usage. Encoding silence is unrealistically cheap, so
 * the test signal is a harmonic stack with noise, at a realistic level.
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

#include "opus.h"
#include "opus_bench.h"

static const char *TAG = "opusbench";

#define MAX_FRAME_MS  40
#define MAX_FS        16000
#define MAX_SAMPLES   (MAX_FS * MAX_FRAME_MS / 1000)   /* 640 at 16 kHz, 40 ms */
#define N_FRAMES      30
#define MAX_PACKET    400

static int16_t s_audio[N_FRAMES * MAX_SAMPLES];    /* generated once per rate */
static int16_t s_out[MAX_SAMPLES];
static uint8_t s_pkt[MAX_PACKET];

/* Speech-like: a few harmonics of a moving fundamental, plus noise.
 *
 * Uses sinf() and float throughout, not sin() and double. The ESP32-S3 FPU is
 * single precision only, so double math is emulated in software - the first
 * version of this called double sin() a few thousand times per frame and
 * starved the idle task badly enough to trip the task watchdog. It never
 * affected the reported timings, which bracket opus_encode() alone, but it did
 * stop the run before it finished.
 *
 * Generated once per sample rate, outside the timed region. */
static void generate_signal(int fs, int total_samples)
{
    float phase = 0.0f, t = 0.0f;
    for (int i = 0; i < total_samples; i++) {
        float f0 = 130.0f * (1.0f + 0.15f * sinf(2.0f * (float)M_PI * 2.0f * t));
        int nharm = (int)((fs * 0.42f) / f0);
        if (nharm > 12) nharm = 12;
        float sum = 0.0f, norm = 0.0f;
        for (int k = 1; k <= nharm; k++) {
            float a = 1.0f / k;
            sum  += a * sinf(phase * k);
            norm += a;
        }
        float noise = 400.0f * (2.0f * (float)rand() / (float)RAND_MAX - 1.0f);
        s_audio[i] = (int16_t)(8000.0f * sum / norm + noise);
        phase += 2.0f * (float)M_PI * f0 / fs;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        t += 1.0f / fs;
    }
}

static void bench_one(int fs, int bitrate, int complexity, int frame_ms)
{
    int err = 0;
    const int frame_samples = fs * frame_ms / 1000;
    const int budget_us = frame_ms * 1000;

    size_t heap_before = esp_get_free_heap_size();

    OpusEncoder *enc = opus_encoder_create(fs, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        ESP_LOGE(TAG, "%5d Hz c%d: encoder_create failed (%s)", fs, complexity,
                 opus_strerror(err));
        return;
    }
    OpusDecoder *dec = opus_decoder_create(fs, 1, &err);
    if (!dec || err != OPUS_OK) {
        ESP_LOGE(TAG, "%5d Hz c%d: decoder_create failed (%s)", fs, complexity,
                 opus_strerror(err));
        opus_encoder_destroy(enc);
        return;
    }

    size_t heap_after = esp_get_free_heap_size();

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));            /* CBR: fixed frame size */
    opus_encoder_ctl(enc, OPUS_SET_DTX(0));

    generate_signal(fs, N_FRAMES * frame_samples);

    int64_t enc_total = 0, enc_max = 0, dec_total = 0, dec_max = 0;
    int bytes_total = 0, bad = 0;

    for (int f = 0; f < N_FRAMES; f++) {
        const int16_t *pcm = &s_audio[f * frame_samples];

        int64_t t0 = esp_timer_get_time();
        int n = opus_encode(enc, pcm, frame_samples, s_pkt, sizeof(s_pkt));
        int64_t dt = esp_timer_get_time() - t0;
        if (n < 0) { bad++; continue; }
        enc_total += dt;
        if (dt > enc_max) enc_max = dt;
        bytes_total += n;

        t0 = esp_timer_get_time();
        int got = opus_decode(dec, s_pkt, n, s_out, frame_samples, 0);
        dt = esp_timer_get_time() - t0;
        if (got != frame_samples) bad++;
        dec_total += dt;
        if (dt > dec_max) dec_max = dt;

        /* Let the idle task run so the watchdog stays fed. Outside the timed
         * regions, so it costs the measurement nothing. */
        if ((f % 4) == 3) vTaskDelay(1);
    }

    int enc_avg = (int)(enc_total / N_FRAMES);
    int dec_avg = (int)(dec_total / N_FRAMES);
    int bytes_avg = bytes_total / N_FRAMES;

    ESP_LOGI(TAG,
             "%5d Hz c%d %2dms %2d kbps | enc %5d us (%2d%%) max %5d | dec %5d us (%2d%%) max %5d | %3d B/frame = %2d kbps wire | state %u B | stack %u B | bad %d",
             fs, complexity, frame_ms, bitrate / 1000,
             enc_avg, 100 * enc_avg / budget_us, (int)enc_max,
             dec_avg, 100 * dec_avg / budget_us, (int)dec_max,
             bytes_avg, (bytes_avg + 4) * 8 * (1000 / frame_ms) / 1000,
             (unsigned)(heap_before - heap_after),
             (unsigned)(40 * 1024 - uxTaskGetStackHighWaterMark(NULL)), bad);

    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
}

static void bench_task(void *arg)
{
    (void)arg;
    srand(1234);

    ESP_LOGI(TAG, "libopus %s, fixed point", opus_get_version_string());
    ESP_LOGI(TAG, "percentages are of that config frame budget; enc and dec run on separate boards");
    ESP_LOGI(TAG, "free heap before: %u B, largest block %u B",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    /* 16 kHz = 8 kHz audio, 12 kHz = 6 kHz audio, 8 kHz = 4 kHz audio.
     * 12 kHz is the interesting middle ground: still half again the bandwidth
     * of Codec 2, at much less than wideband cost. */
    /* 16 kHz = 8 kHz audio, 12 kHz = 6 kHz audio, 8 kHz = 4 kHz audio.
     * Longer frames are worth testing because SILK amortises some per-frame
     * work (LSF quantisation and the like), so 40 ms may cost less than twice
     * a 20 ms frame while giving twice the budget. It costs latency. */
    const int rates[]  = { 16000, 12000, 8000 };
    const int cplx[]   = { 0, 1, 3 };
    const int fms[]    = { 20, 40 };
    for (unsigned f = 0; f < sizeof(fms) / sizeof(fms[0]); f++) {
        for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
            for (unsigned c = 0; c < sizeof(cplx) / sizeof(cplx[0]); c++) {
                bench_one(rates[r], 12000, cplx[c], fms[f]);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }

    ESP_LOGI(TAG, "peak stack used by this task: %u B of %d B",
             (unsigned)(40 * 1024 - uxTaskGetStackHighWaterMark(NULL)), 40 * 1024);
    ESP_LOGI(TAG, "free heap after: %u B", (unsigned)esp_get_free_heap_size());
    ESP_LOGW(TAG, "benchmark done - set TX_OPUS_BENCH to 0 to return to normal operation");

    vTaskDelete(NULL);
}

void opus_bench_run(void)
{
    /* Opus allocates scratch with C99 VLAs (VAR_ARRAYS), so it wants a lot of
     * stack. Give it plenty here and read the high water mark to find out what
     * the real audio task will need. */
    xTaskCreatePinnedToCore(bench_task, "opusbench", 40 * 1024, NULL, 6, NULL, 1);
}
