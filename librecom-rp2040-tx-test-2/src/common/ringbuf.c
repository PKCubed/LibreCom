#include "ringbuf.h"

void sample_ringbuf_init(sample_ringbuf_t *rb, int16_t *storage, size_t capacity) {
    rb->buffer = storage;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

bool sample_ringbuf_push(sample_ringbuf_t *rb, int16_t value) {
    if (rb->count >= rb->capacity) {
        return false;
    }

    rb->buffer[rb->head] = value;
    rb->head = (rb->head + 1u) % rb->capacity;
    rb->count++;
    return true;
}

bool sample_ringbuf_pop(sample_ringbuf_t *rb, int16_t *out_value) {
    if (rb->count == 0u) {
        return false;
    }

    *out_value = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1u) % rb->capacity;
    rb->count--;
    return true;
}

size_t sample_ringbuf_count(const sample_ringbuf_t *rb) {
    return rb->count;
}
