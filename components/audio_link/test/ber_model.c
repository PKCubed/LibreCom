/* Feed 320-byte frames through the parser with random chunk boundaries and a
 * sprinkling of bit errors, to see how frame size interacts with line errors. */
#include "audio_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(int payload_len, double ber, int nframes, int *good)
{
    link_parser_t p;
    link_parser_init(&p, (unsigned short)payload_len);
    unsigned char *frame = malloc(payload_len + 8);
    unsigned char *payload = malloc(payload_len);
    *good = 0;
    for (int f = 0; f < nframes; f++) {
        for (int i = 0; i < payload_len; i++) payload[i] = rand() & 0xFF;
        size_t len = link_build_frame(frame, (unsigned char)f, payload, payload_len);
        for (size_t i = 0; i < len; i++) {
            unsigned char b = frame[i];
            for (int bit = 0; bit < 8; bit++) {
                if ((double)rand() / RAND_MAX < ber) b ^= (1u << bit);
            }
            if (link_parser_feed(&p, b)) (*good)++;
        }
    }
    free(frame); free(payload);
    return p.n_crc_err;
}

int main(void)
{
    srand(20260903);
    printf("%-10s %-12s %-8s %-8s %s\n", "payload", "BER", "sent", "good", "delivered");
    double bers[] = {0.0, 1e-6, 1e-5, 1e-4, 1e-3};
    for (unsigned b = 0; b < sizeof(bers)/sizeof(bers[0]); b++) {
        for (int pl = 8; pl <= 320; pl *= 40) {
            int good = 0;
            run(pl, bers[b], 2000, &good);
            printf("%-10d %-12.0e %-8d %-8d %.1f%%\n", pl, bers[b], 2000, good, 100.0*good/2000);
        }
    }
    return 0;
}
