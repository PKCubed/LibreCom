#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#if __has_include(<codec2/codec2.h>)
#include <codec2/codec2.h>
#elif __has_include(<codec2.h>)
#include <codec2.h>
#else
#error "Codec2 header not found. Ensure Codec2 include path is configured."
#endif

#include "i2s_audio.pio.h"

#define UART_ID uart0
#define UART_RX_PIN 1
#define UART_BAUD 115200

#define I2S_DATA_PIN 8
#define I2S_LRCK_PIN 9
#define I2S_BCK_PIN 10

#define SAMPLE_RATE_HZ 8000
#define I2S_BITS_PER_CHANNEL 32
#define I2S_CHANNELS 2
#define I2S_BCK_HZ (SAMPLE_RATE_HZ * I2S_BITS_PER_CHANNEL * I2S_CHANNELS)

#define DMA_MONO_SAMPLES_PER_BLOCK 128
#define DMA_WORDS_PER_BLOCK (DMA_MONO_SAMPLES_PER_BLOCK * 2)

#define FRAME_SYNC_0 0xA5
#define FRAME_SYNC_1 0x5A

#define UART_RING_SIZE 1024
#define PCM_RING_SIZE 4096

#ifndef CODEC2_MODE_1300
#define CODEC2_MODE_1300 CODEC2_MODE_2400
#endif

static PIO pio_inst = pio0;
static const uint sm_bck = 0;
static const uint sm_lrck = 1;
static const uint sm_data = 2;

static int dma_chan_tx = -1;
static volatile uint32_t dma_tx_buffers[2][DMA_WORDS_PER_BLOCK];
static volatile bool tx_buffer_needs_fill[2];
static volatile uint8_t dma_active_read_idx = 0;

static volatile uint8_t uart_ring[UART_RING_SIZE];
static volatile uint16_t uart_ring_head = 0;
static volatile uint16_t uart_ring_tail = 0;

static int16_t pcm_ring[PCM_RING_SIZE];
static uint16_t pcm_head = 0;
static uint16_t pcm_tail = 0;

static float calc_div_for_square_wave(uint32_t target_hz) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    return (float)sys_hz / (2.0f * (float)target_hz);
}

static bool pcm_ring_push(int16_t v) {
    const uint16_t next = (uint16_t)((pcm_head + 1u) % PCM_RING_SIZE);
    if (next == pcm_tail) {
        return false;
    }
    pcm_ring[pcm_head] = v;
    pcm_head = next;
    return true;
}

static bool pcm_ring_pop(int16_t *v) {
    if (pcm_head == pcm_tail) {
        return false;
    }
    *v = pcm_ring[pcm_tail];
    pcm_tail = (uint16_t)((pcm_tail + 1u) % PCM_RING_SIZE);
    return true;
}

static bool uart_ring_push(uint8_t b) {
    const uint16_t next = (uint16_t)((uart_ring_head + 1u) % UART_RING_SIZE);
    if (next == uart_ring_tail) {
        return false;
    }
    uart_ring[uart_ring_head] = b;
    uart_ring_head = next;
    return true;
}

static bool uart_ring_pop(uint8_t *b) {
    if (uart_ring_head == uart_ring_tail) {
        return false;
    }
    *b = uart_ring[uart_ring_tail];
    uart_ring_tail = (uint16_t)((uart_ring_tail + 1u) % UART_RING_SIZE);
    return true;
}

static uint8_t checksum_bytes(const uint8_t *data, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
    }
    return c;
}

static void __isr uart0_rx_handler(void) {
    while (uart_is_readable(UART_ID)) {
        (void)uart_ring_push((uint8_t)uart_getc(UART_ID));
    }
}

static void fill_dma_tx_buffer(uint8_t idx) {
    volatile uint32_t *dst = dma_tx_buffers[idx];
    for (int i = 0; i < DMA_MONO_SAMPLES_PER_BLOCK; ++i) {
        int16_t s = 0;
        (void)pcm_ring_pop(&s);

        const uint32_t word = ((uint32_t)(uint16_t)s) << 16;
        dst[2 * i] = word;
        dst[(2 * i) + 1] = word;
    }
}

static void init_uart_rx(void) {
    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_fifo_enabled(UART_ID, true);

    uart_set_irq_enables(UART_ID, true, false);
    irq_set_exclusive_handler(UART0_IRQ, uart0_rx_handler);
    irq_set_enabled(UART0_IRQ, true);
}

static void init_bck_sm(uint offset) {
    pio_gpio_init(pio_inst, I2S_BCK_PIN);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_bck, I2S_BCK_PIN, 1, true);

    pio_sm_config c = bck_out_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, I2S_BCK_PIN);
    sm_config_set_clkdiv(&c, calc_div_for_square_wave(I2S_BCK_HZ));

    pio_sm_init(pio_inst, sm_bck, offset, &c);
}

static void init_lrck_sm(uint offset) {
    pio_gpio_init(pio_inst, I2S_LRCK_PIN);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_lrck, I2S_LRCK_PIN, 1, true);

    pio_sm_config c = lrck_from_bck_program_get_default_config(offset);
    sm_config_set_set_pins(&c, I2S_LRCK_PIN, 1);
    sm_config_set_in_pins(&c, I2S_BCK_PIN);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio_inst, sm_lrck, offset, &c);
}

static void init_i2s_tx_sm(uint offset) {
    pio_gpio_init(pio_inst, I2S_DATA_PIN);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_data, I2S_DATA_PIN, 1, true);

    pio_sm_config c = i2s_tx_data_program_get_default_config(offset);
    sm_config_set_out_pins(&c, I2S_DATA_PIN, 1);
    sm_config_set_in_pins(&c, I2S_DATA_PIN);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio_inst, sm_data, offset, &c);
}

static void __isr dma_tx_handler(void) {
    if (!dma_channel_get_irq0_status((uint)dma_chan_tx)) {
        return;
    }

    dma_channel_acknowledge_irq0((uint)dma_chan_tx);

    const uint8_t just_sent = dma_active_read_idx;
    dma_active_read_idx ^= 1u;

    dma_channel_set_read_addr((uint)dma_chan_tx, dma_tx_buffers[dma_active_read_idx], true);
    tx_buffer_needs_fill[just_sent] = true;
}

static void init_i2s_tx_dma(void) {
    dma_chan_tx = dma_claim_unused_channel(true);

    dma_channel_config cfg = dma_channel_get_default_config((uint)dma_chan_tx);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio_inst, sm_data, true));

    fill_dma_tx_buffer(0);
    fill_dma_tx_buffer(1);

    dma_channel_configure((uint)dma_chan_tx,
                          &cfg,
                          &pio_inst->txf[sm_data],
                          dma_tx_buffers[dma_active_read_idx],
                          DMA_WORDS_PER_BLOCK,
                          false);

    dma_channel_set_irq0_enabled((uint)dma_chan_tx, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_tx_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start((uint)dma_chan_tx);
}

static void decode_uart_frames(struct CODEC2 *codec2, int16_t *decoded_pcm, int bytes_per_frame) {
    static enum {
        RX_WAIT_SYNC0,
        RX_WAIT_SYNC1,
        RX_WAIT_LEN,
        RX_WAIT_PAYLOAD,
        RX_WAIT_CRC,
    } state = RX_WAIT_SYNC0;

    static uint8_t payload[64];
    static uint8_t expected_len = 0;
    static uint8_t pos = 0;

    uint8_t b = 0;
    while (uart_ring_pop(&b)) {
        switch (state) {
            case RX_WAIT_SYNC0:
                if (b == FRAME_SYNC_0) {
                    state = RX_WAIT_SYNC1;
                }
                break;

            case RX_WAIT_SYNC1:
                if (b == FRAME_SYNC_1) {
                    state = RX_WAIT_LEN;
                } else {
                    state = RX_WAIT_SYNC0;
                }
                break;

            case RX_WAIT_LEN:
                expected_len = b;
                pos = 0;
                if (expected_len == (uint8_t)bytes_per_frame && expected_len <= sizeof(payload)) {
                    state = RX_WAIT_PAYLOAD;
                } else {
                    state = RX_WAIT_SYNC0;
                }
                break;

            case RX_WAIT_PAYLOAD:
                payload[pos++] = b;
                if (pos >= expected_len) {
                    state = RX_WAIT_CRC;
                }
                break;

            case RX_WAIT_CRC:
                if (checksum_bytes(payload, expected_len) == b) {
                    codec2_decode(codec2, decoded_pcm, payload);
                    const int n = codec2_samples_per_frame(codec2);
                    for (int i = 0; i < n; ++i) {
                        (void)pcm_ring_push(decoded_pcm[i]);
                    }
                }
                state = RX_WAIT_SYNC0;
                break;

            default:
                state = RX_WAIT_SYNC0;
                break;
        }
    }
}

int main(void) {
    stdio_init_all();

    struct CODEC2 *codec2 = codec2_create(CODEC2_MODE_1300);
    if (!codec2) {
        while (true) {
            tight_loop_contents();
        }
    }

    const int samples_per_frame = codec2_samples_per_frame(codec2);
    const int bits_per_frame = codec2_bits_per_frame(codec2);
    const int bytes_per_frame = (bits_per_frame + 7) / 8;

    int16_t *decoded_pcm = (int16_t *)calloc((size_t)samples_per_frame, sizeof(int16_t));
    if (!decoded_pcm || bytes_per_frame > 64) {
        while (true) {
            tight_loop_contents();
        }
    }

    memset((void *)tx_buffer_needs_fill, 0, sizeof(tx_buffer_needs_fill));

    init_uart_rx();

    const uint bck_off = pio_add_program(pio_inst, &bck_out_program);
    const uint lrck_off = pio_add_program(pio_inst, &lrck_from_bck_program);
    const uint tx_off = pio_add_program(pio_inst, &i2s_tx_data_program);

    init_bck_sm(bck_off);
    init_lrck_sm(lrck_off);
    init_i2s_tx_sm(tx_off);

    pio_sm_set_enabled(pio_inst, sm_lrck, true);
    pio_sm_set_enabled(pio_inst, sm_data, true);
    pio_sm_set_enabled(pio_inst, sm_bck, true);

    init_i2s_tx_dma();

    while (true) {
        decode_uart_frames(codec2, decoded_pcm, bytes_per_frame);

        if (tx_buffer_needs_fill[0]) {
            tx_buffer_needs_fill[0] = false;
            fill_dma_tx_buffer(0);
        }

        if (tx_buffer_needs_fill[1]) {
            tx_buffer_needs_fill[1] = false;
            fill_dma_tx_buffer(1);
        }

        tight_loop_contents();
    }
}
