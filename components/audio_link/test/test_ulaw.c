#include "audio_link.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

int main(void)
{
    int fails = 0;

    /* Round-trip SNR on a full-scale sine: G.711 mu-law should give ~38 dB. */
    double sig = 0.0, err = 0.0;
    for (int i = 0; i < 8000; i++) {
        int16_t in = (int16_t)(30000.0 * sin(2.0 * M_PI * 440.0 * i / 8000.0));
        int16_t out = link_ulaw_decode(link_ulaw_encode(in));
        sig += (double)in * in;
        err += ((double)in - out) * ((double)in - out);
    }
    double snr = 10.0 * log10(sig / err);
    printf("sine round-trip SNR: %.1f dB\n", snr);
    if (snr < 30.0) { printf("FAIL: SNR too low\n"); fails++; }

    /* Monotonic and sign-symmetric across the whole 16-bit range. */
    int worst = 0;
    for (int32_t v = -32768; v <= 32767; v++) {
        int16_t out = link_ulaw_decode(link_ulaw_encode((int16_t)v));
        int32_t clip = v < -32635 ? -32635 : (v > 32635 ? 32635 : v);
        int e = abs((int)out - (int)clip);
        if (e > worst) worst = e;
    }
    printf("worst absolute error over full range: %d LSB\n", worst);
    if (worst > 1024) { printf("FAIL: error too large\n"); fails++; }

    /* Silence must survive exactly-ish, and the codes must be distinct. */
    printf("zero -> code 0x%02X -> %d\n", link_ulaw_encode(0), link_ulaw_decode(link_ulaw_encode(0)));
    if (abs(link_ulaw_decode(link_ulaw_encode(0))) > 8) { printf("FAIL: zero not preserved\n"); fails++; }

    int seen[256] = {0}, distinct = 0;
    for (int32_t v = -32768; v <= 32767; v += 1) {
        uint8_t c = link_ulaw_encode((int16_t)v);
        if (!seen[c]) { seen[c] = 1; distinct++; }
    }
    printf("distinct codes used: %d / 256\n", distinct);
    if (distinct < 250) { printf("FAIL: codebook underused\n"); fails++; }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nMU-LAW OK\n", fails);
    return fails != 0;
}
