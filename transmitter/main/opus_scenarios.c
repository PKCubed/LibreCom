/*
 * Opus scenario benchmark: specific mixes of streams, measured rather than
 * added up.
 *
 * Two things this measures that arithmetic cannot:
 *   - worst case per frame, which is what actually decides whether a workload
 *     is safe. The averages have been fine while the worst case sat at 100%.
 *   - the two-core split, where both cores contend for the same SRAM. That
 *     contention is invisible to a sum of single-core timings.
 *
 * Note 12 kbps is narrowband and 16 kbps is wideband; the log says which, so
 * the two cannot be confused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "opus.h"
#include "opus_bench.h"

static const char *TAG = "opusscen";

#define FRAME_MS    20
#define BUDGET_US   (FRAME_MS * 1000)
#define MAX_RATE    48000
#define MAX_SAMPLES (MAX_RATE * FRAME_MS / 1000)
#define MAX_STREAMS 6
#define MAX_PKT     200
#define N_ITER      60

typedef struct {
    int rate;       /* decoder OUTPUT rate                                  */
    int bitrate;    /* bitrate of the stream                                */
    int src_rate;   /* rate it was encoded at; 0 means same as rate         */
} spec_t;

static uint8_t s_pkt[MAX_STREAMS][MAX_PKT];
static int     s_len[MAX_STREAMS];
static int16_t s_src[MAX_SAMPLES];
static int16_t s_o1[MAX_SAMPLES];      /* decoder task output */
static int16_t s_o2[MAX_SAMPLES];      /* encoder task input  */
static int16_t s_mix[MAX_SAMPLES];

static void fill_voice(int16_t *d, int n, int fs, float *ph)
{
    for (int i = 0; i < n; i++) {
        float s = 0.0f, norm = 0.0f;
        for (int k = 1; k <= 12; k++) { float a = 1.0f / k; s += a * sinf(*ph * k); norm += a; }
        d[i] = (int16_t)(8000.0f * s / norm
                         + 300.0f * (2.0f * (float)rand() / (float)RAND_MAX - 1.0f));
        *ph += 2.0f * (float)M_PI * 140.0f / fs;
        if (*ph > 2.0f * (float)M_PI) *ph -= 2.0f * (float)M_PI;
    }
}

static opus_int32 s_seed_bw;

static int make_packet(int fs, int bitrate, uint8_t *out)
{
    int err = 0, n = 0;
    float ph = 0.0f;
    OpusEncoder *e = opus_encoder_create(fs, 1, OPUS_APPLICATION_VOIP, &err);
    if (!e) return -1;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(1));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_VBR(0));
    const int ns = fs * FRAME_MS / 1000;
    for (int f = 0; f < 8; f++) { fill_voice(s_src, ns, fs, &ph); n = opus_encode(e, s_src, ns, out, MAX_PKT); }
    opus_encoder_ctl(e, OPUS_GET_BANDWIDTH(&s_seed_bw));
    opus_encoder_destroy(e);
    return n;
}

/* ---------------- single core ---------------- */
static void scenario(const char *label, int enc_rate, int enc_bitrate,
                     const spec_t *d, int nd)
{
    OpusEncoder *enc = NULL;
    OpusDecoder *dec[MAX_STREAMS] = { 0 };
    int err = 0;
    size_t h0 = esp_get_free_heap_size();

    if (enc_rate) {
        enc = opus_encoder_create(enc_rate, 1, OPUS_APPLICATION_VOIP, &err);
        if (!enc) { ESP_LOGE(TAG, "%s: enc alloc failed", label); return; }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(enc_bitrate));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(1));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    }
    for (int i = 0; i < nd; i++) {
        dec[i] = opus_decoder_create(d[i].rate, 1, &err);
        if (!dec[i]) { ESP_LOGE(TAG, "%s: dec %d alloc failed", label, i); goto out; }
        s_len[i] = make_packet(d[i].src_rate ? d[i].src_rate : d[i].rate, d[i].bitrate, s_pkt[i]);
        if (s_len[i] <= 0) { ESP_LOGE(TAG, "%s: packet %d failed", label, i); goto out; }
    }
    size_t state = h0 - esp_get_free_heap_size();

    int64_t tot = 0, worst = 0;
    float ph = 0.0f;
    const int ens = enc_rate ? enc_rate * FRAME_MS / 1000 : 0;
    for (int it = 0; it < N_ITER; it++) {
        if (enc_rate) fill_voice(s_o2, ens, enc_rate, &ph);
        int64_t t0 = esp_timer_get_time();
        if (enc) { uint8_t t[MAX_PKT]; if (opus_encode(enc, s_o2, ens, t, sizeof t) < 0) {} }
        for (int i = 0; i < nd; i++) {
            int ns = d[i].rate * FRAME_MS / 1000;
            if (opus_decode(dec[i], s_pkt[i], s_len[i], s_o1, ns, 0) != ns) {}
            for (int k = 0; k < ns; k++) {
                int32_t v = s_mix[k] + s_o1[k];
                s_mix[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }
        int64_t dt = esp_timer_get_time() - t0;
        tot += dt; if (dt > worst) worst = dt;
        if ((it % 4) == 3) vTaskDelay(1);
    }
    int avg = (int)(tot / N_ITER);
    ESP_LOGI(TAG, "%-40s %6d us (%3d%%)  worst %6d us (%3d%%)  state %3u KB",
             label, avg, 100 * avg / BUDGET_US,
             (int)worst, (int)(100 * worst / BUDGET_US), (unsigned)(state / 1024));
out:
    if (enc) opus_encoder_destroy(enc);
    for (int i = 0; i < nd; i++) if (dec[i]) opus_decoder_destroy(dec[i]);
}

/* ---------------- two cores, concurrently ---------------- */
static SemaphoreHandle_t s_go_e, s_go_d, s_fin_e, s_fin_d;
static volatile int64_t s_e_tot, s_e_max, s_d_tot, s_d_max;
static int s_enc_rate, s_enc_br, s_nd;
static spec_t s_ds[MAX_STREAMS];

static void enc_core0(void *a)
{
    (void)a; int err = 0; float ph = 0.0f;
    OpusEncoder *e = opus_encoder_create(s_enc_rate, 1, OPUS_APPLICATION_VOIP, &err);
    opus_encoder_ctl(e, OPUS_SET_BITRATE(s_enc_br));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(1));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_VBR(0));
    const int ns = s_enc_rate * FRAME_MS / 1000;
    for (int it = 0; it < N_ITER; it++) {
        xSemaphoreTake(s_go_e, portMAX_DELAY);
        fill_voice(s_o2, ns, s_enc_rate, &ph);
        int64_t t0 = esp_timer_get_time();
        uint8_t t[MAX_PKT];
        if (opus_encode(e, s_o2, ns, t, sizeof t) < 0) {}
        int64_t dt = esp_timer_get_time() - t0;
        s_e_tot += dt; if (dt > s_e_max) s_e_max = dt;
        xSemaphoreGive(s_fin_e);
    }
    opus_encoder_destroy(e);
    vTaskDelete(NULL);
}

static void dec_core1(void *a)
{
    (void)a; int err = 0;
    OpusDecoder *d[MAX_STREAMS] = { 0 };
    for (int i = 0; i < s_nd; i++) d[i] = opus_decoder_create(s_ds[i].rate, 1, &err);
    for (int it = 0; it < N_ITER; it++) {
        xSemaphoreTake(s_go_d, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < s_nd; i++) {
            int ns = s_ds[i].rate * FRAME_MS / 1000;
            if (opus_decode(d[i], s_pkt[i], s_len[i], s_o1, ns, 0) != ns) {}
            for (int k = 0; k < ns; k++) {
                int32_t v = s_mix[k] + s_o1[k];
                s_mix[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }
        int64_t dt = esp_timer_get_time() - t0;
        s_d_tot += dt; if (dt > s_d_max) s_d_max = dt;
        xSemaphoreGive(s_fin_d);
    }
    for (int i = 0; i < s_nd; i++) if (d[i]) opus_decoder_destroy(d[i]);
    vTaskDelete(NULL);
}

static void split(const char *label, int enc_rate, int enc_br, const spec_t *d, int nd)
{
    s_enc_rate = enc_rate; s_enc_br = enc_br; s_nd = nd;
    memcpy(s_ds, d, sizeof(spec_t) * nd);
    for (int i = 0; i < nd; i++) {
        s_len[i] = make_packet(d[i].src_rate ? d[i].src_rate : d[i].rate, d[i].bitrate, s_pkt[i]);
    }
    s_e_tot = s_e_max = s_d_tot = s_d_max = 0;

    xTaskCreatePinnedToCore(enc_core0, "enc0", 40 * 1024, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(dec_core1, "dec1", 40 * 1024, NULL, 6, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    for (int it = 0; it < N_ITER; it++) {
        xSemaphoreGive(s_go_e);
        xSemaphoreGive(s_go_d);
        xSemaphoreTake(s_fin_e, portMAX_DELAY);
        xSemaphoreTake(s_fin_d, portMAX_DELAY);
        if ((it % 4) == 3) vTaskDelay(1);
    }
    int ea = (int)(s_e_tot / N_ITER), da = (int)(s_d_tot / N_ITER);
    ESP_LOGI(TAG, "%-40s core0 enc %5d us (%3d%%) worst %5d (%3d%%) | core1 dec %5d us (%3d%%) worst %5d (%3d%%)",
             label, ea, 100 * ea / BUDGET_US, (int)s_e_max, (int)(100 * s_e_max / BUDGET_US),
             da, 100 * da / BUDGET_US, (int)s_d_max, (int)(100 * s_d_max / BUDGET_US));
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void scen_task(void *a)
{
    (void)a; srand(13579);
    ESP_LOGI(TAG, "20 ms budget = %d us. 12 kbps is narrowband, 16 kbps wideband.", BUDGET_US);
    ESP_LOGI(TAG, "free heap %u B", (unsigned)esp_get_free_heap_size());

    const spec_t n12 = { 16000, 12000, 0 };          /* narrowband */
    const spec_t w16 = { 16000, 16000, 0 };          /* wideband   */
    const spec_t f48 = { 16000, 48000, 48000 };      /* fullband decoded to 16k */
    const spec_t f48_48 = { 48000, 48000, 0 };       /* fullband at 48k out     */
    spec_t s[MAX_STREAMS];

    ESP_LOGI(TAG, "--- ONE CORE, 48 kbps stream decoded to 16 kHz for mixing ---");
    for (int i = 0; i < 4; i++) { s[i] = n12; }
    s[4] = f48;
    scenario("4x12k dec + 48k dec", 0, 0, s, 5);
    for (int i = 0; i < 3; i++) { s[i] = n12; }
    s[3] = f48;
    scenario("1x12k enc + 3x12k dec + 48k dec", 16000, 12000, s, 4);
    for (int i = 0; i < 4; i++) { s[i] = w16; }
    s[4] = f48;
    scenario("4x16k dec + 48k dec", 0, 0, s, 5);
    for (int i = 0; i < 3; i++) { s[i] = w16; }
    s[3] = f48;
    scenario("1x16k enc + 3x16k dec + 48k dec", 16000, 16000, s, 4);

    ESP_LOGI(TAG, "--- ONE CORE, 48 kbps stream at 48 kHz output ---");
    for (int i = 0; i < 3; i++) { s[i] = n12; }
    s[3] = f48_48;
    scenario("1x12k enc + 3x12k dec + 48k@48k", 16000, 12000, s, 4);
    for (int i = 0; i < 3; i++) { s[i] = w16; }
    s[3] = f48_48;
    scenario("1x16k enc + 3x16k dec + 48k@48k", 16000, 16000, s, 4);

    ESP_LOGI(TAG, "--- TWO CORES: encoder on core 0, decoders on core 1 ---");
    for (int i = 0; i < 3; i++) { s[i] = n12; }
    s[3] = f48;
    split("1x12k enc | 3x12k dec + 48k dec", 16000, 12000, s, 4);
    for (int i = 0; i < 3; i++) { s[i] = w16; }
    s[3] = f48;
    split("1x16k enc | 3x16k dec + 48k dec", 16000, 16000, s, 4);

    ESP_LOGI(TAG, "peak stack: %u B of %d B",
             (unsigned)(40 * 1024 - uxTaskGetStackHighWaterMark(NULL)), 40 * 1024);
    ESP_LOGI(TAG, "free heap after: %u B", (unsigned)esp_get_free_heap_size());
    ESP_LOGW(TAG, "scenarios done - set TX_BENCH to 0 for normal operation");
    vTaskDelete(NULL);
}

void opus_scenarios_run(void)
{
    s_go_e  = xSemaphoreCreateBinary();
    s_go_d  = xSemaphoreCreateBinary();
    s_fin_e = xSemaphoreCreateBinary();
    s_fin_d = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(scen_task, "scen", 40 * 1024, NULL, 5, NULL, 1);
}
