#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t transport_encode_audio_packet(
    uint16_t sequence,
    uint16_t sample_count,
    int16_t adpcm_predictor,
    int8_t adpcm_index,
    const uint8_t *adpcm_data,
    size_t adpcm_len,
    uint8_t *out_encoded,
    size_t out_max_len
);

bool transport_decode_audio_packet(
    const uint8_t *encoded_packet,
    size_t encoded_len,
    uint16_t *out_sequence,
    uint16_t *out_sample_count,
    int16_t *out_adpcm_predictor,
    int8_t *out_adpcm_index,
    uint8_t *out_adpcm_data,
    size_t *inout_adpcm_max_len
);
