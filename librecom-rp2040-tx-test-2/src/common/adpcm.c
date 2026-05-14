#include "adpcm.h"

#include <limits.h>

static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

void adpcm_state_init(adpcm_state_t *state) {
    state->predictor = 0;
    state->index = 0;
}

static uint8_t adpcm_encode_nibble(int16_t sample, adpcm_state_t *state) {
    int step = step_table[state->index];
    int diff = sample - state->predictor;
    uint8_t code = 0;

    if (diff < 0) {
        code = 8;
        diff = -diff;
    }

    int delta = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        delta += step;
    }
    if (diff >= (step >> 1)) {
        code |= 2;
        diff -= (step >> 1);
        delta += (step >> 1);
    }
    if (diff >= (step >> 2)) {
        code |= 1;
        delta += (step >> 2);
    }

    if (code & 8) {
        state->predictor -= delta;
    } else {
        state->predictor += delta;
    }

    if (state->predictor > INT16_MAX) {
        state->predictor = INT16_MAX;
    } else if (state->predictor < INT16_MIN) {
        state->predictor = INT16_MIN;
    }

    int new_index = state->index + index_table[code & 0x0F];
    if (new_index < 0) {
        new_index = 0;
    } else if (new_index > 88) {
        new_index = 88;
    }
    state->index = (int8_t)new_index;

    return code & 0x0F;
}

static int16_t adpcm_decode_nibble(uint8_t code, adpcm_state_t *state) {
    int step = step_table[state->index];
    int delta = step >> 3;

    if (code & 4) {
        delta += step;
    }
    if (code & 2) {
        delta += step >> 1;
    }
    if (code & 1) {
        delta += step >> 2;
    }

    if (code & 8) {
        state->predictor -= delta;
    } else {
        state->predictor += delta;
    }

    if (state->predictor > INT16_MAX) {
        state->predictor = INT16_MAX;
    } else if (state->predictor < INT16_MIN) {
        state->predictor = INT16_MIN;
    }

    int new_index = state->index + index_table[code & 0x0F];
    if (new_index < 0) {
        new_index = 0;
    } else if (new_index > 88) {
        new_index = 88;
    }
    state->index = (int8_t)new_index;

    return state->predictor;
}

void adpcm_encode_block(const int16_t *samples, size_t sample_count, uint8_t *encoded, adpcm_state_t *state) {
    size_t out_index = 0;
    for (size_t i = 0; i < sample_count; i += 2) {
        uint8_t lo = adpcm_encode_nibble(samples[i], state);
        uint8_t hi = 0;
        if (i + 1 < sample_count) {
            hi = adpcm_encode_nibble(samples[i + 1], state);
        }
        encoded[out_index++] = (uint8_t)(lo | (hi << 4));
    }
}

void adpcm_decode_block(const uint8_t *encoded, size_t sample_count, int16_t *samples, adpcm_state_t *state) {
    size_t out_index = 0;
    for (size_t i = 0; i < (sample_count + 1u) / 2u; ++i) {
        uint8_t packed = encoded[i];
        uint8_t lo = packed & 0x0F;
        uint8_t hi = (packed >> 4) & 0x0F;

        samples[out_index++] = adpcm_decode_nibble(lo, state);
        if (out_index < sample_count) {
            samples[out_index++] = adpcm_decode_nibble(hi, state);
        }
    }
}
