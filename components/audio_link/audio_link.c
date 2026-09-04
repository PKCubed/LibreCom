#include "audio_link.h"

#include <string.h>

/* Parser states */
enum {
    ST_SYNC0 = 0,
    ST_SYNC1,
    ST_SEQ,
    ST_LEN_HI,
    ST_LEN_LO,
    ST_PAYLOAD,
    ST_CRC,
};

uint8_t link_crc8_update(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int b = 0; b < 8; b++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint8_t link_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc = link_crc8_update(crc, data[i]);
    }
    return crc;
}

/* ------------------------------------------------------------------ *
 *  G.711 mu-law
 * ------------------------------------------------------------------ */
#define ULAW_BIAS 0x84
#define ULAW_CLIP 32635

uint8_t link_ulaw_encode(int16_t sample)
{
    int32_t s = sample;
    uint8_t sign = (s < 0) ? 0x80u : 0x00u;

    if (s < 0) {
        s = -s;                     /* int32 math, so INT16_MIN is safe here */
    }
    if (s > ULAW_CLIP) {
        s = ULAW_CLIP;
    }
    s += ULAW_BIAS;

    int exponent = 7;
    for (int32_t mask = 0x4000; (s & mask) == 0 && exponent > 0; exponent--, mask >>= 1) {
    }
    int32_t mantissa = (s >> (exponent + 3)) & 0x0F;

    return (uint8_t)(~(sign | (uint8_t)(exponent << 4) | (uint8_t)mantissa));
}

int16_t link_ulaw_decode(uint8_t code)
{
    uint8_t u = (uint8_t)~code;
    int32_t exponent = (u >> 4) & 0x07;
    int32_t mantissa = u & 0x0F;
    int32_t magnitude = (((mantissa << 3) + ULAW_BIAS) << exponent) - ULAW_BIAS;

    return (int16_t)((u & 0x80) ? -magnitude : magnitude);
}

size_t link_build_frame(uint8_t *out, uint8_t seq,
                        const uint8_t *payload, size_t payload_len)
{
    out[0] = LINK_SYNC0;
    out[1] = LINK_SYNC1;
    out[2] = seq;
    out[3] = (uint8_t)(payload_len >> 8);
    out[4] = (uint8_t)(payload_len & 0xFF);
    memcpy(&out[5], payload, payload_len);
    /* CRC over seq + len + payload, i.e. everything after the sync word. */
    out[5 + payload_len] = link_crc8(&out[2], payload_len + 3);
    return LINK_FRAME_LEN(payload_len);
}

void link_parser_init(link_parser_t *p, uint16_t max_payload)
{
    memset(p, 0, sizeof(*p));
    p->max_payload = max_payload > LINK_MAX_PAYLOAD ? LINK_MAX_PAYLOAD : max_payload;
    p->state = ST_SYNC0;
}

bool link_parser_feed(link_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case ST_SYNC0:
        if (b == LINK_SYNC0) {
            p->state = ST_SYNC1;
        }
        break;

    case ST_SYNC1:
        if (b == LINK_SYNC1) {
            p->state = ST_SEQ;
        } else if (b == LINK_SYNC0) {
            /* stay put: 0xA5 0xA5 0x5A is still a valid frame start */
        } else {
            p->state = ST_SYNC0;
        }
        break;

    case ST_SEQ:
        p->seq   = b;
        p->crc   = link_crc8_update(0x00, b);
        p->state = ST_LEN_HI;
        break;

    case ST_LEN_HI:
        p->payload_len = (uint16_t)b << 8;
        p->crc = link_crc8_update(p->crc, b);
        p->state = ST_LEN_LO;
        break;

    case ST_LEN_LO:
        /* A length we could never have sent means this was not a real frame
         * start - almost certainly the sync word appearing inside a payload.
         * Drop it and hunt again rather than swallowing max_payload bytes. */
        p->payload_len |= b;
        if (p->payload_len == 0 || p->payload_len > p->max_payload) {
            p->n_badlen++;
            p->n_resync++;
            p->state = ST_SYNC0;
            break;
        }
        p->crc = link_crc8_update(p->crc, b);
        p->idx = 0;
        p->state = ST_PAYLOAD;
        break;

    case ST_PAYLOAD:
        p->payload[p->idx++] = b;
        p->crc = link_crc8_update(p->crc, b);
        if (p->idx >= p->payload_len) {
            p->state = ST_CRC;
        }
        break;

    case ST_CRC:
        p->state = ST_SYNC0;
        if (p->crc != b) {
            /* Either a corrupted frame or a false sync inside payload bytes.
             * Both are handled the same way: drop it and hunt for sync again. */
            p->n_crc_err++;
            p->n_resync++;
            return false;
        }
        if (p->have_last_seq) {
            p->gap = (uint8_t)(p->seq - p->last_seq - 1);
            p->n_lost += p->gap;
        } else {
            p->gap = 0;
            p->have_last_seq = true;
        }
        p->last_seq = p->seq;
        p->n_good++;
        return true;

    default:
        p->state = ST_SYNC0;
        break;
    }

    return false;
}
