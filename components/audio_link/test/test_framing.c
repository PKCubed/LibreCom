#include "audio_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if(!(c)) { printf("FAIL: %s\n", msg); fails++; } } while(0)

int main(void)
{
    const int N = 8;
    link_parser_t p;
    link_parser_init(&p, 320);

    uint8_t frame[LINK_MAX_PAYLOAD + LINK_FRAME_OVERHEAD];
    uint8_t payload[8];

    /* 1. Clean round trip of 1000 random frames. */
    srand(1234);
    int accepted = 0;
    for (int f = 0; f < 1000; f++) {
        for (int i = 0; i < N; i++) payload[i] = rand() & 0xFF;
        size_t len = link_build_frame(frame, (uint8_t)f, payload, N);
        CHECK(len == (size_t)(N + LINK_FRAME_OVERHEAD), "frame length");
        for (size_t i = 0; i < len; i++) {
            if (link_parser_feed(&p, frame[i])) {
                accepted++;
                CHECK(memcmp(p.payload, payload, N) == 0, "payload mismatch");
                CHECK(p.gap == 0, "unexpected gap");
            }
        }
    }
    CHECK(accepted == 1000, "all clean frames accepted");
    CHECK(p.n_crc_err == 0, "no crc errors on clean stream");
    CHECK(p.n_lost == 0, "no losses on clean stream");
    printf("clean stream: accepted=%d crc_err=%u lost=%u\n", accepted, p.n_crc_err, p.n_lost);

    /* 2. Payload that contains the sync pattern must still parse. */
    link_parser_init(&p, 320);
    uint8_t evil[8] = {0xA5, 0x5A, 0x00, 0xA5, 0x5A, 0xFF, 0xA5, 0x5A};
    size_t len = link_build_frame(frame, 7, evil, N);
    accepted = 0;
    for (size_t i = 0; i < len; i++) if (link_parser_feed(&p, frame[i])) accepted++;
    CHECK(accepted == 1, "sync-in-payload frame accepted");
    CHECK(memcmp(p.payload, evil, N) == 0, "sync-in-payload contents");

    /* 3. A corrupted byte must be rejected, and the parser must recover. */
    link_parser_init(&p, 320);
    for (int i = 0; i < N; i++) payload[i] = (uint8_t)(i * 7);
    len = link_build_frame(frame, 1, payload, N);
    frame[5] ^= 0xFF;                       /* corrupt one payload byte */
    for (size_t i = 0; i < len; i++) link_parser_feed(&p, frame[i]);
    CHECK(p.n_crc_err == 1, "corruption detected");
    CHECK(p.n_good == 0, "corrupted frame not accepted");

    len = link_build_frame(frame, 2, payload, N);   /* next frame is clean */
    accepted = 0;
    for (size_t i = 0; i < len; i++) if (link_parser_feed(&p, frame[i])) accepted++;
    CHECK(accepted == 1, "parser recovered after corruption");

    /* 4. Dropped frames are reported as a gap. */
    link_parser_init(&p, 320);
    len = link_build_frame(frame, 10, payload, N);
    for (size_t i = 0; i < len; i++) link_parser_feed(&p, frame[i]);
    len = link_build_frame(frame, 13, payload, N);  /* 11 and 12 went missing */
    accepted = 0;
    for (size_t i = 0; i < len; i++) if (link_parser_feed(&p, frame[i])) accepted++;
    CHECK(accepted == 1, "post-gap frame accepted");
    CHECK(p.gap == 2, "gap of 2 reported");
    CHECK(p.n_lost == 2, "lost counter");

    /* 5. Sequence wrap 255 -> 0 must not look like a 255-frame gap. */
    link_parser_init(&p, 320);
    len = link_build_frame(frame, 255, payload, N);
    for (size_t i = 0; i < len; i++) link_parser_feed(&p, frame[i]);
    len = link_build_frame(frame, 0, payload, N);
    for (size_t i = 0; i < len; i++) link_parser_feed(&p, frame[i]);
    CHECK(p.gap == 0, "seq wrap is not a gap");

    /* 6. Resync from mid-stream garbage. */
    link_parser_init(&p, 320);
    uint8_t junk[37];
    for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof(junk); i++) link_parser_feed(&p, junk[i]);
    len = link_build_frame(frame, 42, payload, N);
    accepted = 0;
    for (size_t i = 0; i < len; i++) if (link_parser_feed(&p, frame[i])) accepted++;
    CHECK(accepted == 1, "resync after garbage");


    /* 7. 320-byte payload (the raw-PCM bring-up mode). */
    {
        const int NP = 320;
        static uint8_t big[320], fr[320 + LINK_FRAME_OVERHEAD];
        link_parser_t q;
        link_parser_init(&q, 320);
        int acc = 0;
        for (int f = 0; f < 200; f++) {
            for (int i = 0; i < NP; i++) big[i] = rand() & 0xFF;
            size_t l = link_build_frame(fr, (uint8_t)f, big, NP);
            for (size_t i = 0; i < l; i++)
                if (link_parser_feed(&q, fr[i])) {
                    acc++;
                    CHECK(memcmp(q.payload, big, NP) == 0, "320B payload contents");
                }
        }
        CHECK(acc == 200, "all 320B frames accepted");
        CHECK(q.n_crc_err == 0, "no crc errors on 320B stream");
        printf("320B stream: accepted=%d crc_err=%u\n", acc, q.n_crc_err);
    }


    /* 8. Variable-length frames on a single stream - what Opus produces. */
    {
        link_parser_t v;
        link_parser_init(&v, 320);
        static uint8_t big[320], fr[320 + LINK_FRAME_OVERHEAD];
        int acc = 0, sizes[] = { 30, 31, 29, 60, 1, 320, 12 };
        for (unsigned k = 0; k < sizeof(sizes)/sizeof(sizes[0]); k++) {
            int L = sizes[k];
            for (int i = 0; i < L; i++) big[i] = (uint8_t)(rand() & 0xFF);
            size_t l = link_build_frame(fr, (uint8_t)k, big, L);
            CHECK(l == (size_t)(L + LINK_FRAME_OVERHEAD), "variable frame length");
            for (size_t i = 0; i < l; i++) {
                if (link_parser_feed(&v, fr[i])) {
                    acc++;
                    CHECK(v.payload_len == L, "reported payload_len");
                    CHECK(memcmp(v.payload, big, L) == 0, "variable payload contents");
                }
            }
        }
        CHECK(acc == (int)(sizeof(sizes)/sizeof(sizes[0])), "all variable frames accepted");
        printf("variable-length stream: accepted=%d of %d, crc_err=%u badlen=%u\n",
               acc, (int)(sizeof(sizes)/sizeof(sizes[0])), v.n_crc_err, v.n_badlen);
    }

    /* 9. An implausible length must be rejected without swallowing the stream. */
    {
        link_parser_t v;
        link_parser_init(&v, 64);            /* this end accepts at most 64 */
        static uint8_t big[320], fr[320 + LINK_FRAME_OVERHEAD];
        for (int i = 0; i < 320; i++) big[i] = (uint8_t)i;
        size_t l = link_build_frame(fr, 1, big, 320);   /* far too long for us */
        int acc = 0;
        for (size_t i = 0; i < l; i++) if (link_parser_feed(&v, fr[i])) acc++;
        CHECK(acc == 0, "oversized frame rejected");
        CHECK(v.n_badlen >= 1, "bad length counted");

        /* and the parser must still find the next good frame */
        l = link_build_frame(fr, 2, big, 40);
        acc = 0;
        for (size_t i = 0; i < l; i++) if (link_parser_feed(&v, fr[i])) acc++;
        CHECK(acc == 1, "recovered after an oversized frame");
        printf("oversize rejection: badlen=%u, recovered=%s\n",
               v.n_badlen, acc == 1 ? "yes" : "no");
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails != 0;
}
