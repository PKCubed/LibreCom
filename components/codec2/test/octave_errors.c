/*
 * Measures how often Codec 2's pitch estimator makes octave errors.
 *
 * An octave error is when nlp() returns roughly 2x the true fundamental for a
 * frame or two. The decoder resynthesises that frame an octave high, and it is
 * heard as the voice cracking up a register for a few milliseconds.
 *
 * The estimator is driven directly with a synthetic voiced signal at a known
 * F0, so the truth is exact and the error rate is a measurement rather than an
 * impression. Build with -DCNLP=... and -DCODEC2_PITCH_MAX_HZ=... to sweep the
 * two settings that affect it; see run_octave_test.sh.
 */
#include "defines.h"
#include "sine.h"
#include "nlp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void *codec2_malloc(size_t n)           { return malloc(n); }
void *codec2_calloc(size_t a, size_t b) { return calloc(a, b); }
void  codec2_free(void *p)              { free(p); }

/* CNLP lives inside nlp.c; mirror its default so we can report what we built with. */
#ifndef CNLP
#define CNLP 0.3
#endif
#ifndef CODEC2_PITCH_MAX_HZ
#define CODEC2_PITCH_MAX_HZ 400
#endif

#define FS        8000
#define N_SAMP    80          /* 10 ms subframe, as mode 3200 uses */
#define WARMUP    12          /* frames discarded while filters settle */

/* A voiced-speech proxy. Two details matter for provoking octave errors:
 *
 *  - h1_atten weakens the fundamental relative to its harmonics. Real speech
 *    does this constantly (high-pass response, closed-quotient variation,
 *    formants well above F0), and it is the condition under which the global
 *    peak of the pitch likelihood function lands on 2*F0 instead of F0.
 *  - F0 is not constant. A slow vibrato keeps the estimator tracking rather
 *    than locked onto a stationary tone, which is how it behaves on speech.
 */
static void synth(float *out, int n, float f0, double *phase, double *t,
                  float noise_amp, float h1_atten)
{
    for (int i = 0; i < n; i++) {
        float f0_now = f0 * (1.0f + 0.04f * sinf(2.0f * (float)M_PI * 3.0f * (float)*t));
        int nharm = (int)(3400.0f / f0_now);
        if (nharm < 1) nharm = 1;

        double s = 0.0, norm = 0.0;
        for (int k = 1; k <= nharm; k++) {
            double a = 1.0 / k;
            if (k == 1) a *= h1_atten;        /* weak fundamental */
            s    += a * sin(*phase * k);
            norm += a;
        }
        double noise = noise_amp * (2.0 * rand() / RAND_MAX - 1.0);
        out[i] = (float)(9000.0 * s / (norm > 0.0 ? norm : 1.0) + noise);

        *phase += 2.0 * M_PI * f0_now / FS;
        if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
        *t += 1.0 / FS;
    }
}

static double measure(float noise_amp, float h1_atten,
                      int *n_frames_out, double *other_err_out)
{
    C2CONST c2const = c2const_create(FS, 0.01f);
    int m_pitch = c2const.m_pitch;

    float *Sn  = calloc(m_pitch, sizeof(float));
    float *blk = calloc(N_SAMP, sizeof(float));
    static COMP  Sw[FFT_ENC];        /* nlp() does not read these */
    static float W[FFT_ENC];

    int total = 0, octave_up = 0, other = 0;

    for (float f0 = 90.0f; f0 <= 260.0f; f0 += 5.0f) {
        void *nlp_state = nlp_create(&c2const);
        float prev_f0 = 100.0f;
        double phase = 0.0, t = 0.0;
        memset(Sn, 0, m_pitch * sizeof(float));

        for (int frame = 0; frame < 40; frame++) {
            synth(blk, N_SAMP, f0, &phase, &t, noise_amp, h1_atten);
            memmove(Sn, Sn + N_SAMP, (m_pitch - N_SAMP) * sizeof(float));
            memcpy(Sn + m_pitch - N_SAMP, blk, N_SAMP * sizeof(float));

            float pitch = 0.0f;
            float est = nlp(nlp_state, Sn, N_SAMP, &pitch, Sw, W, &prev_f0);
            prev_f0 = est;

            if (frame < WARMUP) continue;
            total++;
            double ratio = est / f0;
            if (fabs(ratio - 2.0) < 0.25)      octave_up++;
            else if (fabs(ratio - 1.0) > 0.10) other++;
        }
        nlp_destroy(nlp_state);
    }

    free(Sn); free(blk);
    *n_frames_out = total;
    *other_err_out = 100.0 * other / total;
    return 100.0 * octave_up / total;
}

int main(void)
{
    srand(12345);
    printf("CNLP=%.2f  pitch ceiling=%d Hz\n", (double)CNLP, (int)CODEC2_PITCH_MAX_HZ);
    printf("  %-22s %-8s %-12s %s\n", "condition", "frames", "octave-up", "other error");

    struct { const char *name; float noise; float h1; } cases[] = {
        { "strong fundamental",   200.0f, 1.00f },
        { "H1 -10 dB",            200.0f, 0.32f },
        { "H1 -20 dB",            200.0f, 0.10f },
        { "H1 -30 dB",            200.0f, 0.03f },
        { "H1 -20 dB + noise",   1500.0f, 0.10f },
    };
    double worst = 0.0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int frames; double other;
        double up = measure(cases[i].noise, cases[i].h1, &frames, &other);
        if (up > worst) worst = up;
        printf("  %-22s %-8d %-12.2f %.2f %%\n", cases[i].name, frames, up, other);
    }
    printf("  worst octave-up rate: %.2f %%\n", worst);
    return 0;
}
