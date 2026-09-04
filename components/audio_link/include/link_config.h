/*
 * Settings that MUST be identical on both boards.
 * Edit here only, then rebuild and reflash BOTH transmitter and receiver.
 */
#pragma once

/* Payload carried in each frame:
 *   0 = Opus (the real thing)
 *   1 = raw PCM (bring-up aid: takes the codec out of the picture entirely so
 *       you can test the wire, the DAC and the jitter buffer on their own).
 */
#define LINK_SEND_PCM        0

/* Only matters when LINK_SEND_PCM is 1. 1 = 8-bit mu-law, 0 = 16-bit linear. */
#define LINK_PCM_ULAW        1

/* The boards are cross-wired GPIO4 -> GPIO5 in both directions, so there are
 * two physical jumpers and the audio only uses one. Flipping this moves the
 * link onto the other one - the quickest way to find a dead wire. */
#define LINK_SWAP_PINS       0

/* ------------------------------------------------------------------ *
 *  Audio format
 *
 *  16 kHz gives Opus 8 kHz of audio bandwidth, against the 3.6 kHz Codec 2
 *  was limited to. Measured on an ESP32-S3 at 240 MHz, encoding costs 34% of
 *  the frame budget at complexity 1 and decoding 7%, so wideband is close to
 *  free here - the expensive setting is complexity, not sample rate.
 * ------------------------------------------------------------------ */
#define LINK_SAMPLE_RATE     16000
#define LINK_FRAME_MS        20
#define LINK_FRAME_SAMPLES   (LINK_SAMPLE_RATE * LINK_FRAME_MS / 1000)   /* 320 */
#define LINK_FRAMES_PER_SEC  (1000 / LINK_FRAME_MS)

/* ------------------------------------------------------------------ *
 *  Opus
 * ------------------------------------------------------------------ */
/* Opus refuses wideband below about 14 kbps and silently encodes narrowband
 * instead - which is exactly as muffled as Codec 2 was, and was the real cause
 * of the "Opus still sounds muffled" report. Measured on the host with this
 * project's own libopus build, complexity 1, CBR:
 *
 *     12 kbps -> narrowband 4 kHz     14 kbps -> wideband 8 kHz
 *
 * Complexity is part of the decision: Opus scales its internal equivalent rate
 * by (90 + complexity)/100, so the low complexity that makes wideband
 * affordable on this chip also pushes the rate under the threshold. 12 kbps at
 * complexity 5 does pick wideband, but costs 77% of the frame budget instead
 * of 34%.
 *
 * 24 kbps sits well clear of the threshold and uses under a third of the UART.
 * The transmitter warns if Opus ever drops below wideband, so a future change
 * that pushes it back under cannot go unnoticed. */
#define LINK_OPUS_BITRATE    24000
/* Complexity 2 and above switches SILK to its delayed-decision quantiser,
 * which costs more than twice as much: measured 34% of budget at complexity 1
 * against 77% at complexity 3, for very little quality. Do not raise this
 * without re-running components/../opus_bench and checking the headroom. */
#define LINK_OPUS_COMPLEXITY 1
/* Opus chooses its own bandwidth from the bitrate. At 12 kbps it may well
 * decide the bits are better spent on a narrower band - and if it settles on
 * narrowband, feeding it 16 kHz audio buys nothing at all and it will sound
 * exactly as muffled as Codec 2 did. The transmitter logs what it actually
 * chose once a second.
 *
 * 0 = let Opus decide (measure first).
 * Otherwise one of OPUS_BANDWIDTH_MEDIUMBAND / _WIDEBAND, forced.
 * Forcing wideband at a bitrate too low for it trades clarity for bandwidth;
 * raising LINK_OPUS_BITRATE is usually the better fix, and there is plenty of
 * room on the wire - 12 kbps only uses 16% of the UART. */
#define LINK_OPUS_FORCE_BW   0

/* CBR at 12 kbps produces 30 bytes per 20 ms frame. The cap is generous so a
 * VBR experiment or a bitrate change does not silently truncate frames. */
#define LINK_OPUS_MAX_BYTES  200

/* ------------------------------------------------------------------ *
 *  Wire
 * ------------------------------------------------------------------ */
#if LINK_SEND_PCM
#  if LINK_PCM_ULAW
#    define LINK_PAYLOAD_MAX (LINK_FRAME_SAMPLES)        /* 320 B at 16 kHz  */
#    define LINK_UART_BAUD   460800
#  else
#    define LINK_PAYLOAD_MAX (LINK_FRAME_SAMPLES * 2)    /* 640 B at 16 kHz  */
#    define LINK_UART_BAUD   460800
#  endif
#else
#  define LINK_PAYLOAD_MAX   LINK_OPUS_MAX_BYTES
#  define LINK_UART_BAUD     115200
#endif

/* Worst-case bytes per second on the wire, for the boot banner and for the
 * receiver to compare its measured byte rate against. Opus is CBR here so the
 * real figure matches; in PCM modes the payload is fixed anyway. */
#if LINK_SEND_PCM
#  define LINK_WIRE_BYTES_PER_SEC ((LINK_PAYLOAD_MAX + 6) * LINK_FRAMES_PER_SEC)
#else
#  define LINK_WIRE_BYTES_PER_SEC ((LINK_OPUS_BITRATE / 8 / LINK_FRAMES_PER_SEC + 6) \
                                   * LINK_FRAMES_PER_SEC)
#endif
