/*
 * Settings that MUST be identical on both boards.
 * Edit here only, then rebuild and reflash BOTH transmitter and receiver.
 */
#pragma once

/* Payload carried in each frame:
 *   0 = Codec 2 bits  (the real thing, 8 bytes per 20 ms)
 *   1 = raw PCM (bring-up aid: takes Codec 2 out of the picture entirely so
 *       you can test the wire, the DAC and the jitter buffer on their own).
 */
#define LINK_SEND_PCM        0

/* Only matters when LINK_SEND_PCM is 1.
 *
 *   1 = compand to 8-bit mu-law. 160 byte payload, 82 kbit/s, so this runs at
 *       the SAME 115200 baud as the Codec 2 mode. Keep it here: it means the
 *       PCM stage and the Codec 2 stage put identical electrical demands on
 *       the wire, so if one works and the other does not, the wire is not the
 *       variable. Costs about 39 dB SNR, which is inaudible on a test tone.
 *
 *   0 = 16-bit linear. 320 byte payload, 162 kbit/s, needs 460800 baud.
 *       Only use this when you specifically want to test the high baud rate:
 *       a 324-byte frame is 27x more likely to catch a bit error than a
 *       12-byte one, so a marginal wire that carries Codec 2 fine can drop
 *       essentially every linear-PCM frame.
 */
#define LINK_PCM_ULAW        1

/* The boards are cross-wired GPIO4 -> GPIO5 in both directions, so there are
 * two physical jumpers and the audio only ever uses one of them. Flipping this
 * moves the audio link onto the other jumper, which is the quickest way to
 * find out whether one specific wire or crimp is dead. Must match on both
 * boards, which is why it lives here rather than in the two main.c files. */
#define LINK_SWAP_PINS       0

#define LINK_SAMPLE_RATE     8000
#define LINK_FRAME_SAMPLES   160    /* CODEC2_MODE_3200 -> 20 ms per frame */
#define LINK_C2_BYTES        8      /* 64 bits per frame                   */
#define LINK_FRAME_MS        (LINK_FRAME_SAMPLES * 1000 / LINK_SAMPLE_RATE)
#define LINK_FRAMES_PER_SEC  (1000 / LINK_FRAME_MS)

#if LINK_SEND_PCM
#  if LINK_PCM_ULAW
/* 164 bytes x 50/s x 10 bits = 82 kbit/s out of 115200. */
#    define LINK_PAYLOAD_LEN LINK_FRAME_SAMPLES
#    define LINK_UART_BAUD   115200
#  else
/* 324 bytes x 50/s x 10 bits = 162 kbit/s, so 460800 leaves ~2.8x headroom. */
#    define LINK_PAYLOAD_LEN (LINK_FRAME_SAMPLES * 2)
#    define LINK_UART_BAUD   460800
#  endif
#else
/* 12 bytes x 50/s x 10 bits = 6 kbit/s. 115200 is 19x more than we need. */
#  define LINK_PAYLOAD_LEN   LINK_C2_BYTES
#  define LINK_UART_BAUD     115200
#endif

/* Bytes per second actually on the wire, logged at boot on both sides so a
 * mismatch is obvious at a glance. */
#define LINK_WIRE_BYTES_PER_SEC ((LINK_PAYLOAD_LEN + 4) * LINK_FRAMES_PER_SEC)
