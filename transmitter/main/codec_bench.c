/*
 * Cross-codec comparison: Codec 2, Opus and AMR-NB on identical axes.
 *
 * Everything here is measured on the chip. The one number we never had is
 * algorithmic delay, which is measured directly: feed silence, then a loud
 * tone burst, encode and decode the whole thing, and find how far the burst
 * has moved by the time it comes out the far side. That is the codec's own
 * contribution to mouth-to-ear latency, separate from any buffering we add.
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

#include "codec2.h"
#include "opus.h"
#include "interf_enc.h"
#include "interf_dec.h"
#include "opus_bench.h"

static const char *TAG = "codecs";

/* Each codec is measured in its own task. uxTaskGetStackHighWaterMark only ever
 * falls, so a single shared task would report the deepest codec so far for
 * every row after it - which is what the first run did. */
static SemaphoreHandle_t s_done;
static int s_which;

#define MAX_FS        16000
#define MAX_FRAME_MS  20
#define MAX_SAMPLES   (MAX_FS * MAX_FRAME_MS / 1000)   /* 320 */
#define N_TIME        40                                /* frames for timing   */
#define N_SILENCE     15                                /* frames before burst */
#define N_BURST       15                                /* frames of burst     */
#define N_DELAY       (N_SILENCE + N_BURST)
#define MAX_PKT       200
#define STACK_BYTES   (40 * 1024)

static int16_t s_in[N_DELAY * MAX_SAMPLES];
static int16_t s_dec[N_DELAY * MAX_SAMPLES];
static int16_t s_speech[N_TIME * MAX_SAMPLES];
static uint8_t s_pkt[MAX_PKT];

/* Speech-like signal for the timing runs. */
static void gen_speech(int fs, int n)
{
    float phase = 0.0f, t = 0.0f;
    for (int i = 0; i < n; i++) {
        float f0 = 130.0f * (1.0f + 0.15f * sinf(2.0f * (float)M_PI * 2.0f * t));
        int nh = (int)((fs * 0.42f) / f0);
        if (nh > 12) nh = 12;
        float sum = 0.0f, norm = 0.0f;
        for (int k = 1; k <= nh; k++) { float a = 1.0f / k; sum += a * sinf(phase * k); norm += a; }
        s_speech[i] = (int16_t)(8000.0f * sum / norm
                                + 300.0f * (2.0f * (float)rand() / (float)RAND_MAX - 1.0f));
        phase += 2.0f * (float)M_PI * f0 / fs;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        t += 1.0f / fs;
    }
}

/* Silence, then a loud steady tone. The step is what we time. */
static void gen_burst(int fs, int frame_samples)
{
    memset(s_in, 0, sizeof(s_in));
    float phase = 0.0f;
    const int start = N_SILENCE * frame_samples;
    for (int i = start; i < N_DELAY * frame_samples; i++) {
        s_in[i] = (int16_t)(12000.0f * sinf(phase));
        phase += 2.0f * (float)M_PI * 400.0f / fs;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
}

/* First output sample whose local energy passes a tenth of the burst level. */
static int find_onset(int total_samples, int frame_samples)
{
    const int win = 32;
    int64_t peak = 0;
    for (int i = 0; i + win < total_samples; i += win) {
        int64_t e = 0;
        for (int k = 0; k < win; k++) e += (int64_t)s_dec[i + k] * s_dec[i + k];
        if (e > peak) peak = e;
    }
    if (peak == 0) return -1;
    const int64_t thresh = peak / 100;         /* -20 dB from the burst plateau */
    for (int i = 0; i + win < total_samples; i += win) {
        int64_t e = 0;
        for (int k = 0; k < win; k++) e += (int64_t)s_dec[i + k] * s_dec[i + k];
        if (e > thresh) return i;
    }
    return -1;
}

static void report(const char *name, int fs, int frame_ms, int frame_samples,
                   int64_t enc_tot, int64_t enc_max, int64_t dec_tot, int64_t dec_max,
                   int bytes, size_t state, int onset)
{
    const int budget = frame_ms * 1000;
    int enc = (int)(enc_tot / N_TIME), dec = (int)(dec_tot / N_TIME);
    int delay_ms = (onset < 0) ? -1
                 : (onset - N_SILENCE * frame_samples) * 1000 / fs;
    if (onset >= 0 && delay_ms < 0) {
        ESP_LOGW(TAG, "%s: onset at sample %d is BEFORE the burst at %d - "
                      "decoder state was not clean", name, onset,
                 N_SILENCE * frame_samples);
    }
    ESP_LOGI(TAG,
             "%-22s %5d Hz %2dms | enc %5d us %3d%% (max %5d) | dec %5d us %3d%% (max %5d) | %3d B/fr %3d kbps wire | state %5u B | delay %3d ms | stack %5u B",
             name, fs, frame_ms,
             enc, 100 * enc / budget, (int)enc_max,
             dec, 100 * dec / budget, (int)dec_max,
             bytes, (bytes + 6) * 8 * (1000 / frame_ms) / 1000,
             (unsigned)state, delay_ms,
             (unsigned)(STACK_BYTES - uxTaskGetStackHighWaterMark(NULL)));
}

/* ------------------------------------------------------------------ */
static void bench_codec2(void)
{
    const int fs = 8000, frame_ms = 20, ns = 160;
    size_t h0 = esp_get_free_heap_size();
    struct CODEC2 *e = codec2_create(CODEC2_MODE_3200);
    struct CODEC2 *d = codec2_create(CODEC2_MODE_3200);
    if (!e || !d) { ESP_LOGE(TAG, "codec2_create failed"); return; }
    size_t state = h0 - esp_get_free_heap_size();
    const int bytes = (codec2_bits_per_frame(e) + 7) / 8;

    gen_speech(fs, N_TIME * ns);
    int64_t et = 0, em = 0, dt_ = 0, dm = 0;
    unsigned char bits[16];
    int16_t out[160];
    for (int f = 0; f < N_TIME; f++) {
        int64_t t0 = esp_timer_get_time();
        codec2_encode(e, bits, &s_speech[f * ns]);
        int64_t x = esp_timer_get_time() - t0; et += x; if (x > em) em = x;
        t0 = esp_timer_get_time();
        codec2_decode(d, out, bits);
        x = esp_timer_get_time() - t0; dt_ += x; if (x > dm) dm = x;
        if ((f % 8) == 7) vTaskDelay(1);
    }

    /* Fresh state: the timing loop above left speech in the decoder, and its
     * tail would trip the onset detector at sample zero. */
    codec2_destroy(e); codec2_destroy(d);
    e = codec2_create(CODEC2_MODE_3200);
    d = codec2_create(CODEC2_MODE_3200);
    gen_burst(fs, ns);
    for (int f = 0; f < N_DELAY; f++) {
        codec2_encode(e, bits, &s_in[f * ns]);
        codec2_decode(d, &s_dec[f * ns], bits);
    }
    report("Codec 2 3200", fs, frame_ms, ns, et, em, dt_, dm, bytes, state,
           find_onset(N_DELAY * ns, ns));
    codec2_destroy(e); codec2_destroy(d);
}

static void bench_opus(int fs, int bitrate, int cplx)
{
    const int frame_ms = 20, ns = fs * frame_ms / 1000;
    int err = 0;
    size_t h0 = esp_get_free_heap_size();
    OpusEncoder *e = opus_encoder_create(fs, 1, OPUS_APPLICATION_VOIP, &err);
    OpusDecoder *d = opus_decoder_create(fs, 1, &err);
    if (!e || !d) { ESP_LOGE(TAG, "opus create failed"); return; }
    size_t state = h0 - esp_get_free_heap_size();
    opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(cplx));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_VBR(0));

    gen_speech(fs, N_TIME * ns);
    int64_t et = 0, em = 0, dt_ = 0, dm = 0;
    int bytes = 0, bad = 0;
    static int16_t out[MAX_SAMPLES];
    for (int f = 0; f < N_TIME; f++) {
        int64_t t0 = esp_timer_get_time();
        int n = opus_encode(e, &s_speech[f * ns], ns, s_pkt, MAX_PKT);
        int64_t x = esp_timer_get_time() - t0; et += x; if (x > em) em = x;
        if (n > 0) bytes = n;
        t0 = esp_timer_get_time();
        if (opus_decode(d, s_pkt, n, out, ns, 0) != ns) bad++;
        x = esp_timer_get_time() - t0; dt_ += x; if (x > dm) dm = x;
        if ((f % 8) == 7) vTaskDelay(1);
    }

    /* Fresh state, same reason as above. */
    opus_encoder_destroy(e); opus_decoder_destroy(d);
    e = opus_encoder_create(fs, 1, OPUS_APPLICATION_VOIP, &err);
    d = opus_decoder_create(fs, 1, &err);
    opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(cplx));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_VBR(0));
    gen_burst(fs, ns);
    for (int f = 0; f < N_DELAY; f++) {
        int n = opus_encode(e, &s_in[f * ns], ns, s_pkt, MAX_PKT);
        if (opus_decode(d, s_pkt, n, &s_dec[f * ns], ns, 0) != ns) bad++;
    }
    opus_int32 bw = 0;
    opus_encoder_ctl(e, OPUS_GET_BANDWIDTH(&bw));
    char name[32];
    snprintf(name, sizeof(name), "Opus %d kbps c%d %s", bitrate / 1000, cplx,
             bw == OPUS_BANDWIDTH_WIDEBAND ? "WB" :
             bw == OPUS_BANDWIDTH_MEDIUMBAND ? "MB" :
             bw == OPUS_BANDWIDTH_NARROWBAND ? "NB" : "?");
    if (bad) ESP_LOGW(TAG, "%s: %d decode errors", name, bad);
    report(name, fs, frame_ms, ns, et, em, dt_, dm, bytes, state,
           find_onset(N_DELAY * ns, ns));
    opus_encoder_destroy(e); opus_decoder_destroy(d);
}

static void bench_amr(void)
{
    const int fs = 8000, frame_ms = 20, ns = 160;
    size_t h0 = esp_get_free_heap_size();
    void *e = Encoder_Interface_init(0);
    void *d = Decoder_Interface_init();
    if (!e || !d) { ESP_LOGE(TAG, "amr init failed"); return; }
    size_t state = h0 - esp_get_free_heap_size();

    gen_speech(fs, N_TIME * ns);
    int64_t et = 0, em = 0, dt_ = 0, dm = 0;
    int bytes = 0;
    int16_t out[160];
    for (int f = 0; f < N_TIME; f++) {
        int64_t t0 = esp_timer_get_time();
        int n = Encoder_Interface_Encode(e, MR122, &s_speech[f * ns], s_pkt, 0);
        int64_t x = esp_timer_get_time() - t0; et += x; if (x > em) em = x;
        if (n > 0) bytes = n;
        t0 = esp_timer_get_time();
        Decoder_Interface_Decode(d, s_pkt, out, 0);
        x = esp_timer_get_time() - t0; dt_ += x; if (x > dm) dm = x;
        if ((f % 8) == 7) vTaskDelay(1);
    }

    /* Fresh state, same reason as above. */
    Encoder_Interface_exit(e); Decoder_Interface_exit(d);
    e = Encoder_Interface_init(0);
    d = Decoder_Interface_init();
    gen_burst(fs, ns);
    for (int f = 0; f < N_DELAY; f++) {
        Encoder_Interface_Encode(e, MR122, &s_in[f * ns], s_pkt, 0);
        Decoder_Interface_Decode(d, s_pkt, &s_dec[f * ns], 0);
    }
    report("AMR-NB 12.2 kbps", fs, frame_ms, ns, et, em, dt_, dm, bytes, state,
           find_onset(N_DELAY * ns, ns));
    Encoder_Interface_exit(e); Decoder_Interface_exit(d);
}

static void one_codec(void *arg)
{
    (void)arg;
    switch (s_which) {
    case 0: bench_codec2();            break;
    case 1: bench_opus(16000, 12000, 1); break;
    case 2: bench_opus(16000, 16000, 1); break;
    case 3: bench_opus(16000, 24000, 1); break;
    case 4: bench_opus(8000,  12000, 1); break;
    case 5: bench_amr();               break;
    default: break;
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void codec_task(void *arg)
{
    (void)arg;
    srand(97531);
    ESP_LOGI(TAG, "all three codecs, same axes, 20 ms frames, 240 MHz");
    ESP_LOGI(TAG, "percentages are of the 20000 us frame budget");
    ESP_LOGI(TAG, "delay = codec algorithmic delay only, measured from a tone burst");
    ESP_LOGI(TAG, "free heap %u B", (unsigned)esp_get_free_heap_size());

    for (s_which = 0; s_which < 6; s_which++) {
        xTaskCreatePinnedToCore(one_codec, "codec1", STACK_BYTES, NULL, 6, NULL, 1);
        xSemaphoreTake(s_done, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    ESP_LOGI(TAG, "free heap after: %u B", (unsigned)esp_get_free_heap_size());
    ESP_LOGW(TAG, "comparison done - set TX_BENCH to 0 for normal operation");
    vTaskDelete(NULL);
}

void codec_bench_run(void)
{
    s_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(codec_task, "codecs", 4 * 1024, NULL, 6, NULL, 1);
}
