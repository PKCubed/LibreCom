/*
 * Opus multi-stream load benchmark.
 *
 * Answers "how many streams fit on one ESP32-S3". Rather than adding up
 * single-stream timings, it builds each target workload for real and times the
 * whole thing against one 20 ms frame period, because that is what has to
 * complete before the next frame arrives.
 *
 * Also reports heap, since decoder state is what runs out first when you scale
 * stream count rather than bitrate.
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

static const char *TAG = "opusload";

#define FRAME_MS      20
#define BUDGET_US     (FRAME_MS * 1000)
#define MAX_RATE      48000
#define MAX_SAMPLES   (MAX_RATE * FRAME_MS / 1000)   /* 960 at 48 kHz */
#define MAX_STREAMS   6
#define MAX_PKT       200
#define N_ITER        50

/* One period of pre-encoded packets per stream, so the timed loop only ever
 * measures decoding. */
static uint8_t  s_pkt[MAX_STREAMS][MAX_PKT];
static int      s_pkt_len[MAX_STREAMS];
static int16_t  s_src[MAX_SAMPLES];
static int16_t  s_out[MAX_SAMPLES];
static int16_t  s_mix[MAX_SAMPLES];

static void fill_voice(int16_t *dst, int n, int fs, float *phase, float *t)
{
    for (int i = 0; i < n; i++) {
        float f0 = 140.0f * (1.0f + 0.12f * sinf(2.0f * (float)M_PI * 2.0f * *t));
        int nharm = (int)((fs * 0.42f) / f0);
        if (nharm > 14) nharm = 14;
        float sum = 0.0f, norm = 0.0f;
        for (int k = 1; k <= nharm; k++) {
            float a = 1.0f / k;
            sum  += a * sinf(*phase * k);
            norm += a;
        }
        dst[i] = (int16_t)(8000.0f * sum / norm
                           + 300.0f * (2.0f * (float)rand() / (float)RAND_MAX - 1.0f));
        *phase += 2.0f * (float)M_PI * f0 / fs;
        if (*phase > 2.0f * (float)M_PI) *phase -= 2.0f * (float)M_PI;
        *t += 1.0f / fs;
    }
}

/* Encode one representative packet at the given rate and bitrate. Used only to
 * produce realistic input for the decoders; never timed. */
static const char *bwname(opus_int32 bw)
{
    return bw == OPUS_BANDWIDTH_NARROWBAND    ? "NB" :
           bw == OPUS_BANDWIDTH_MEDIUMBAND    ? "MB" :
           bw == OPUS_BANDWIDTH_WIDEBAND      ? "WB" :
           bw == OPUS_BANDWIDTH_SUPERWIDEBAND ? "SWB" :
           bw == OPUS_BANDWIDTH_FULLBAND      ? "FB" : "?";
}

static opus_int32 s_last_bw;

static int make_packet(int fs, int bitrate, uint8_t *out, int outmax)
{
    int err = 0, n = 0;
    OpusEncoder *e = opus_encoder_create(fs, 1, OPUS_APPLICATION_VOIP, &err);
    if (!e || err != OPUS_OK) return -1;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(1));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_VBR(0));
    float ph = 0.0f, t = 0.0f;
    const int ns = fs * FRAME_MS / 1000;
    for (int f = 0; f < 8; f++) {           /* let the encoder settle */
        fill_voice(s_src, ns, fs, &ph, &t);
        n = opus_encode(e, s_src, ns, out, outmax);
    }
    opus_encoder_ctl(e, OPUS_GET_BANDWIDTH(&s_last_bw));
    opus_encoder_destroy(e);
    return n;
}

/* A workload: one optional encoder, plus a list of decoders. */
typedef struct {
    int rate;       /* decoder OUTPUT rate                                  */
    int bitrate;    /* bitrate of the stream feeding it                     */
    int src_rate;   /* rate the source was encoded at; 0 means same as rate */
} dec_spec_t;

static void bench_load(const char *label, int enc_rate, int enc_bitrate,
                       const dec_spec_t *decs, int n_dec)
{
    OpusEncoder *enc = NULL;
    OpusDecoder *dec[MAX_STREAMS] = { 0 };
    int err = 0;

    size_t heap_before = esp_get_free_heap_size();

    if (enc_rate) {
        enc = opus_encoder_create(enc_rate, 1, OPUS_APPLICATION_VOIP, &err);
        if (!enc) { ESP_LOGE(TAG, "%s: encoder alloc failed", label); return; }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(enc_bitrate));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(1));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    }
    for (int i = 0; i < n_dec; i++) {
        dec[i] = opus_decoder_create(decs[i].rate, 1, &err);
        if (!dec[i]) {
            ESP_LOGE(TAG, "%s: decoder %d alloc failed - out of heap", label, i);
            goto cleanup;
        }
        int src = decs[i].src_rate ? decs[i].src_rate : decs[i].rate;
        s_pkt_len[i] = make_packet(src, decs[i].bitrate, s_pkt[i], MAX_PKT);
        if (s_pkt_len[i] <= 0) {
            ESP_LOGE(TAG, "%s: could not build a test packet for stream %d", label, i);
            goto cleanup;
        }
    }

    size_t heap_after = esp_get_free_heap_size();

    int64_t total = 0, worst = 0;
    int bad = 0;
    float ph = 0.0f, t = 0.0f;
    const int enc_ns = enc_rate ? enc_rate * FRAME_MS / 1000 : 0;

    for (int it = 0; it < N_ITER; it++) {
        if (enc_rate) fill_voice(s_src, enc_ns, enc_rate, &ph, &t);

        int64_t t0 = esp_timer_get_time();

        if (enc) {
            uint8_t tmp[MAX_PKT];
            int en = opus_encode(enc, s_src, enc_ns, tmp, sizeof(tmp));
            if (en < 0) bad++;
        }
        for (int i = 0; i < n_dec; i++) {
            int ns = decs[i].rate * FRAME_MS / 1000;
            if (opus_decode(dec[i], s_pkt[i], s_pkt_len[i], s_out, ns, 0) != ns) bad++;
            /* Mixing is part of the real workload, so include it. */
            for (int k = 0; k < ns; k++) {
                int32_t v = s_mix[k] + s_out[k];
                s_mix[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }

        int64_t dt = esp_timer_get_time() - t0;
        total += dt;
        if (dt > worst) worst = dt;

        if ((it % 4) == 3) vTaskDelay(1);
    }

    int avg = (int)(total / N_ITER);
    ESP_LOGI(TAG, "%-34s %6d us avg (%3d%%)  worst %6d us (%3d%%)  state %3u KB  streams are %s  stack %u B",
             label, avg, 100 * avg / BUDGET_US,
             (int)worst, (int)(100 * worst / BUDGET_US),
             (unsigned)((heap_before - heap_after) / 1024), bwname(s_last_bw),
             (unsigned)(48 * 1024 - uxTaskGetStackHighWaterMark(NULL)));
    if (bad) ESP_LOGW(TAG, "  %d codec errors during that run", bad);

cleanup:
    if (enc) opus_encoder_destroy(enc);
    for (int i = 0; i < n_dec; i++) if (dec[i]) opus_decoder_destroy(dec[i]);
}

static void load_task(void *arg)
{
    (void)arg;
    srand(4321);

    ESP_LOGI(TAG, "one 20 ms frame period is %d us; percentages are of that", BUDGET_US);
    ESP_LOGI(TAG, "free heap %u B, largest block %u B",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "everything below runs on ONE core, sequentially in one task");

    /* 24 kbps at 16 kHz is what the boards actually run, and it is the only
     * one of these that Opus encodes as wideband. 12 kbps is kept for
     * comparison but is narrowband - the log says which, so the two cannot be
     * confused the way they were earlier. */
    const dec_spec_t wb24  = { 16000, 24000, 0 };
    const dec_spec_t nb12  = { 16000, 12000, 0 };
    const dec_spec_t fb48  = { 48000, 48000, 0 };       /* fullband, 48 kHz out */
    const dec_spec_t fb48_16 = { 16000, 48000, 48000 }; /* fullband in, 16 kHz out */

    dec_spec_t set[MAX_STREAMS];

    ESP_LOGI(TAG, "--- single operations ---");
    set[0] = wb24;    bench_load("1 decode  24kbps 16k out",  0, 0, set, 1);
    set[0] = nb12;    bench_load("1 decode  12kbps 16k out",  0, 0, set, 1);
    set[0] = fb48;    bench_load("1 decode  48kbps 48k out",  0, 0, set, 1);
    set[0] = fb48_16; bench_load("1 decode  48kbps 16k out",  0, 0, set, 1);
    bench_load("1 encode  24kbps", 16000, 24000, set, 0);

    ESP_LOGI(TAG, "--- target workloads at the shipping config (24 kbps WB) ---");
    for (int i = 0; i < 4; i++) set[i] = wb24;
    bench_load("4 decode",            0, 0, set, 4);
    bench_load("1 encode + 3 decode", 16000, 24000, set, 3);

    ESP_LOGI(TAG, "--- same at 12 kbps (narrowband, for comparison) ---");
    for (int i = 0; i < 4; i++) set[i] = nb12;
    bench_load("4 decode",            0, 0, set, 4);
    bench_load("1 encode + 3 decode", 16000, 12000, set, 3);

    ESP_LOGI(TAG, "--- plus a fifth stream at 48 kbps ---");
    for (int i = 0; i < 3; i++) set[i] = wb24;
    set[3] = fb48_16;
    bench_load("1 enc + 3 dec + 48kbps@16k out", 16000, 24000, set, 4);
    set[3] = fb48;
    bench_load("1 enc + 3 dec + 48kbps@48k out", 16000, 24000, set, 4);

    ESP_LOGI(TAG, "--- headroom: decoders at the shipping config ---");
    for (int n = 4; n <= MAX_STREAMS; n++) {
        char l[48];
        for (int i = 0; i < n; i++) set[i] = wb24;
        snprintf(l, sizeof(l), "%d decode 24kbps WB", n);
        bench_load(l, 0, 0, set, n);
    }

    ESP_LOGI(TAG, "peak stack for this task: %u B of %d B",
             (unsigned)(48 * 1024 - uxTaskGetStackHighWaterMark(NULL)), 48 * 1024);
    ESP_LOGI(TAG, "free heap after: %u B", (unsigned)esp_get_free_heap_size());
    ESP_LOGW(TAG, "load benchmark done - set TX_BENCH to 0 for normal operation");
    vTaskDelete(NULL);
}

void opus_load_run(void)
{
    xTaskCreatePinnedToCore(load_task, "opusload", 48 * 1024, NULL, 6, NULL, 1);
}
