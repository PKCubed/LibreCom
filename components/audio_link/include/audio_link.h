/*
 * audio_link - byte-oriented framing for the UART hop between the two ESP32s.
 *
 * Wire format:
 *
 *   +------+------+------+--------+--------+--------------+------+
 *   | 0xA5 | 0x5A | seq  | len_hi | len_lo | payload[len] | crc8 |
 *   +------+------+------+--------+--------+--------------+------+
 *
 * crc8 covers seq + len + payload (CRC-8/ATM, poly 0x07, init 0x00).
 * The length is 16-bit: one byte would cap the payload at 255, and the raw
 * PCM bring-up modes need 320 bytes (mu-law at 16 kHz) or 640 (16-bit).
 * seq increments once per frame so the receiver can tell a dropped frame
 * apart from a corrupted one and conceal the gap.
 *
 * The length is on the wire because Opus frames are not a fixed size: even in
 * CBR the encoder may emit a shorter frame, and DTX would emit far shorter
 * ones. Codec 2 never did this, so earlier versions of this protocol agreed
 * the size at compile time.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_SYNC0           0xA5u
#define LINK_SYNC1           0x5Au
#define LINK_MAX_PAYLOAD     768   /* covers 16-bit PCM at 16 kHz (640 B) */
#define LINK_FRAME_OVERHEAD  6
#define LINK_FRAME_LEN(n)    ((n) + LINK_FRAME_OVERHEAD)

uint8_t link_crc8(const uint8_t *data, size_t len);

/* G.711 mu-law companding. Squeezes a 16-bit sample into 8 bits with roughly
 * 38 dB SNR across the range, which halves the wire rate of raw PCM. Used by
 * the raw-PCM bring-up mode so it can run at the same baud as the Codec 2
 * mode instead of needing four times the line rate. */
uint8_t link_ulaw_encode(int16_t sample);
int16_t link_ulaw_decode(uint8_t code);

/* Folds one byte into a running CRC-8/ATM. Start from 0x00. */
uint8_t link_crc8_update(uint8_t crc, uint8_t byte);

/* Serialises one frame into out[], which must hold LINK_FRAME_LEN(payload_len)
 * bytes. Returns the number of bytes written. */
size_t link_build_frame(uint8_t *out, uint8_t seq,
                        const uint8_t *payload, size_t payload_len);

typedef struct {
    uint16_t max_payload;       /* largest frame we will accept            */
    uint16_t payload_len;       /* length of the frame just received       */
    uint8_t  state;
    uint16_t idx;
    uint8_t  seq;               /* seq of the frame currently being parsed */
    uint8_t  crc;               /* running CRC over seq + payload           */
    uint8_t  last_seq;          /* seq of the last frame accepted          */
    bool     have_last_seq;
    uint8_t  payload[LINK_MAX_PAYLOAD];

    /* Set by link_parser_feed() when it returns true. Number of frames the
     * sender emitted between the previous accepted frame and this one. */
    uint8_t  gap;

    /* Cumulative counters, for logging. Reset them yourself. */
    uint32_t n_good;
    uint32_t n_crc_err;
    uint32_t n_lost;            /* frames implied missing by seq gaps */
    uint32_t n_resync;          /* times the parser fell back to sync hunting */
    uint32_t n_badlen;          /* frames whose length field was implausible   */
} link_parser_t;

/* max_payload is the largest frame this end will accept; anything longer is
 * treated as a bad sync and dropped. */
void link_parser_init(link_parser_t *p, uint16_t max_payload);

/* Feed one received byte. Returns true when p->payload holds a complete,
 * CRC-checked frame (and p->gap says how many frames were lost before it). */
bool link_parser_feed(link_parser_t *p, uint8_t b);

#ifdef __cplusplus
}
#endif
