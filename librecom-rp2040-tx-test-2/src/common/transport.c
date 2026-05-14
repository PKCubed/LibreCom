#include "transport.h"

#include "config.h"

#include <string.h>

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static size_t cobs_encode(const uint8_t *input, size_t length, uint8_t *output, size_t out_max) {
    if (out_max == 0u) {
        return 0u;
    }

    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

    while (read_index < length) {
        if (write_index >= out_max) {
            return 0u;
        }

        if (input[read_index] == 0) {
            output[code_index] = code;
            code = 1;
            code_index = write_index++;
            read_index++;
        } else {
            output[write_index++] = input[read_index++];
            code++;
            if (code == 0xFF) {
                output[code_index] = code;
                code = 1;
                code_index = write_index++;
                if (write_index > out_max) {
                    return 0u;
                }
            }
        }
    }

    if (code_index >= out_max) {
        return 0u;
    }
    output[code_index] = code;
    return write_index;
}

static size_t cobs_decode(const uint8_t *input, size_t length, uint8_t *output, size_t out_max) {
    size_t read_index = 0;
    size_t write_index = 0;

    while (read_index < length) {
        uint8_t code = input[read_index];
        if (code == 0) {
            return 0u;
        }
        read_index++;

        for (uint8_t i = 1; i < code; ++i) {
            if (read_index >= length || write_index >= out_max) {
                return 0u;
            }
            output[write_index++] = input[read_index++];
        }

        if (code != 0xFF && read_index < length) {
            if (write_index >= out_max) {
                return 0u;
            }
            output[write_index++] = 0;
        }
    }

    return write_index;
}

size_t transport_encode_audio_packet(
    uint16_t sequence,
    uint16_t sample_count,
    int16_t adpcm_predictor,
    int8_t adpcm_index,
    const uint8_t *adpcm_data,
    size_t adpcm_len,
    uint8_t *out_encoded,
    size_t out_max_len
) {
    uint8_t raw[TRANSPORT_MAX_PACKET_BYTES];
    if (adpcm_len > TRANSPORT_MAX_ADPCM_BYTES) {
        return 0u;
    }

    size_t raw_len = 0;
    raw[raw_len++] = TRANSPORT_PACKET_TYPE_AUDIO;
    raw[raw_len++] = (uint8_t)(sequence & 0xFFu);
    raw[raw_len++] = (uint8_t)((sequence >> 8) & 0xFFu);
    raw[raw_len++] = (uint8_t)(sample_count & 0xFFu);
    raw[raw_len++] = (uint8_t)((sample_count >> 8) & 0xFFu);
    raw[raw_len++] = (uint8_t)(adpcm_len & 0xFFu);
    raw[raw_len++] = (uint8_t)(adpcm_predictor & 0xFF);
    raw[raw_len++] = (uint8_t)((adpcm_predictor >> 8) & 0xFF);
    raw[raw_len++] = (uint8_t)adpcm_index;

    if (raw_len + adpcm_len + 2u > sizeof(raw)) {
        return 0u;
    }

    memcpy(&raw[raw_len], adpcm_data, adpcm_len);
    raw_len += adpcm_len;

    uint16_t crc = crc16_ccitt(raw, raw_len);
    raw[raw_len++] = (uint8_t)(crc & 0xFFu);
    raw[raw_len++] = (uint8_t)((crc >> 8) & 0xFFu);

    size_t encoded_len = cobs_encode(raw, raw_len, out_encoded, out_max_len);
    if (encoded_len == 0u || encoded_len >= out_max_len) {
        return 0u;
    }

    out_encoded[encoded_len++] = 0u;
    return encoded_len;
}

bool transport_decode_audio_packet(
    const uint8_t *encoded_packet,
    size_t encoded_len,
    uint16_t *out_sequence,
    uint16_t *out_sample_count,
    int16_t *out_adpcm_predictor,
    int8_t *out_adpcm_index,
    uint8_t *out_adpcm_data,
    size_t *inout_adpcm_max_len
) {
    if (encoded_len == 0u || *inout_adpcm_max_len == 0u) {
        return false;
    }

    uint8_t raw[TRANSPORT_MAX_PACKET_BYTES];
    size_t raw_len = cobs_decode(encoded_packet, encoded_len, raw, sizeof(raw));
    if (raw_len < 11u) {
        return false;
    }

    uint16_t received_crc = (uint16_t)raw[raw_len - 2u] | ((uint16_t)raw[raw_len - 1u] << 8);
    uint16_t calculated_crc = crc16_ccitt(raw, raw_len - 2u);
    if (received_crc != calculated_crc) {
        return false;
    }

    if (raw[0] != TRANSPORT_PACKET_TYPE_AUDIO) {
        return false;
    }

    uint16_t sequence = (uint16_t)raw[1] | ((uint16_t)raw[2] << 8);
    uint16_t sample_count = (uint16_t)raw[3] | ((uint16_t)raw[4] << 8);
    size_t adpcm_len = raw[5];
    int16_t predictor = (int16_t)((uint16_t)raw[6] | ((uint16_t)raw[7] << 8));
    int8_t index = (int8_t)raw[8];

    if (9u + adpcm_len + 2u != raw_len) {
        return false;
    }

    if (adpcm_len > *inout_adpcm_max_len) {
        return false;
    }

    memcpy(out_adpcm_data, &raw[9], adpcm_len);
    *inout_adpcm_max_len = adpcm_len;
    *out_sequence = sequence;
    *out_sample_count = sample_count;
    *out_adpcm_predictor = predictor;
    *out_adpcm_index = index;
    return true;
}
