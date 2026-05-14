#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t *buffer;
    size_t capacity;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t count;
} sample_ringbuf_t;

void sample_ringbuf_init(sample_ringbuf_t *rb, int16_t *storage, size_t capacity);
bool sample_ringbuf_push(sample_ringbuf_t *rb, int16_t value);
bool sample_ringbuf_pop(sample_ringbuf_t *rb, int16_t *out_value);
size_t sample_ringbuf_count(const sample_ringbuf_t *rb);
