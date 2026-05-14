#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t predictor;
    int8_t index;
} adpcm_state_t;

void adpcm_state_init(adpcm_state_t *state);

void adpcm_encode_block(const int16_t *samples, size_t sample_count, uint8_t *encoded, adpcm_state_t *state);
void adpcm_decode_block(const uint8_t *encoded, size_t sample_count, int16_t *samples, adpcm_state_t *state);
