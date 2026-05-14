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

#include "i2s_audio.pio.h"

#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_BAUD 230400

#define I2S_DATA_PIN 8
#define I2S_LRCK_PIN 9
#define I2S_BCK_PIN 10
#define I2S_MCLK_PIN 11

#define SAMPLE_RATE_HZ 8000
#define I2S_BITS_PER_CHANNEL 32
#define I2S_CHANNELS 2
#define I2S_BCK_HZ (SAMPLE_RATE_HZ * I2S_BITS_PER_CHANNEL * I2S_CHANNELS)
#define I2S_MCLK_HZ (SAMPLE_RATE_HZ * 256)

#define DMA_WORDS_PER_BLOCK 256
#define PCM_FRAME_SAMPLES 160
#define PCM_FRAME_BYTES (PCM_FRAME_SAMPLES * sizeof(int16_t))

#define FRAME_SYNC_0 0xA5
#define FRAME_SYNC_1 0x5A

static PIO pio_inst = pio0;
static const uint sm_bck = 0;
static const uint sm_lrck = 1;
static const uint sm_data = 2;
static const uint sm_mclk = 3;

static int dma_chan_rx = -1;
static volatile uint32_t dma_rx_buffers[2][DMA_WORDS_PER_BLOCK];
static volatile bool dma_block_ready[2];
static volatile uint8_t dma_active_write_idx = 0;

static float calc_div_for_square_wave(uint32_t target_hz) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    return (float)sys_hz / (2.0f * (float)target_hz);
}

static void init_uart_tx(void) {
    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_fifo_enabled(UART_ID, true);
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

static void init_i2s_rx_sm(uint offset) {
    pio_gpio_init(pio_inst, I2S_DATA_PIN);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_data, I2S_DATA_PIN, 1, false);

    pio_sm_config c = i2s_rx_data_program_get_default_config(offset);
    sm_config_set_in_pins(&c, I2S_DATA_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(pio_inst, sm_data, offset, &c);
}

static void init_mclk_sm(uint offset) {
    pio_gpio_init(pio_inst, I2S_MCLK_PIN);
    pio_sm_set_consecutive_pindirs(pio_inst, sm_mclk, I2S_MCLK_PIN, 1, true);

    pio_sm_config c = mclk_out_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, I2S_MCLK_PIN);
    sm_config_set_clkdiv(&c, calc_div_for_square_wave(I2S_MCLK_HZ));

    pio_sm_init(pio_inst, sm_mclk, offset, &c);
}

static void __isr dma_rx_handler(void) {
    if (!dma_channel_get_irq0_status((uint)dma_chan_rx)) {
        return;
    }

    dma_channel_acknowledge_irq0((uint)dma_chan_rx);
    dma_block_ready[dma_active_write_idx] = true;

    dma_active_write_idx ^= 1u;
    dma_channel_set_write_addr((uint)dma_chan_rx, dma_rx_buffers[dma_active_write_idx], true);
}

static void init_i2s_rx_dma(void) {
    dma_chan_rx = dma_claim_unused_channel(true);

    dma_channel_config cfg = dma_channel_get_default_config((uint)dma_chan_rx);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio_inst, sm_data, false));

    dma_channel_configure((uint)dma_chan_rx,
                          &cfg,
                          dma_rx_buffers[dma_active_write_idx],
                          &pio_inst->rxf[sm_data],
                          DMA_WORDS_PER_BLOCK,
                          false);

    dma_channel_set_irq0_enabled((uint)dma_chan_rx, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_rx_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start((uint)dma_chan_rx);
}

static uint8_t checksum_bytes(const uint8_t *data, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
    }
    return c;
}

static void uart_send_frame(const uint8_t *payload, uint16_t len) {
    uart_putc_raw(UART_ID, FRAME_SYNC_0);
    uart_putc_raw(UART_ID, FRAME_SYNC_1);
    uart_putc_raw(UART_ID, (uint8_t)(len & 0xFFu));
    uart_putc_raw(UART_ID, (uint8_t)(len >> 8));

    for (uint16_t i = 0; i < len; ++i) {
        uart_putc_raw(UART_ID, payload[i]);
    }

    uart_putc_raw(UART_ID, checksum_bytes(payload, len));
}

int main(void) {
    stdio_init_all();

    int16_t *pcm_frame = (int16_t *)calloc(PCM_FRAME_SAMPLES, sizeof(int16_t));
    uint8_t *payload = (uint8_t *)calloc(PCM_FRAME_BYTES, sizeof(uint8_t));
    if (!pcm_frame || !payload) {
        while (true) {
            tight_loop_contents();
        }
    }

    memset((void *)dma_block_ready, 0, sizeof(dma_block_ready));

    init_uart_tx();

    const uint bck_off = pio_add_program(pio_inst, &bck_out_program);
    const uint lrck_off = pio_add_program(pio_inst, &lrck_from_bck_program);
    const uint rx_off = pio_add_program(pio_inst, &i2s_rx_data_program);
    const uint mclk_off = pio_add_program(pio_inst, &mclk_out_program);

    init_bck_sm(bck_off);
    init_lrck_sm(lrck_off);
    init_i2s_rx_sm(rx_off);
    init_mclk_sm(mclk_off);

    pio_sm_set_enabled(pio_inst, sm_lrck, true);
    pio_sm_set_enabled(pio_inst, sm_data, true);
    pio_sm_set_enabled(pio_inst, sm_mclk, true);
    pio_sm_set_enabled(pio_inst, sm_bck, true);

    init_i2s_rx_dma();

    int frame_pos = 0;
    while (true) {
        uint8_t ready_idx = 0xFFu;
        if (dma_block_ready[0]) {
            ready_idx = 0;
        } else if (dma_block_ready[1]) {
            ready_idx = 1;
        }

        if (ready_idx == 0xFFu) {
            tight_loop_contents();
            continue;
        }

        dma_block_ready[ready_idx] = false;
        const volatile uint32_t *raw = dma_rx_buffers[ready_idx];

        for (int i = 0; i < DMA_WORDS_PER_BLOCK; i += 2) {
            int32_t s32 = (int32_t)raw[i];
            int16_t sample = (int16_t)(s32 >> 16);
            pcm_frame[frame_pos++] = sample;

            if (frame_pos == PCM_FRAME_SAMPLES) {
                for (int j = 0; j < PCM_FRAME_SAMPLES; ++j) {
                    const uint16_t sample_bits = (uint16_t)pcm_frame[j];
                    payload[(j * 2) + 0] = (uint8_t)(sample_bits & 0xFFu);
                    payload[(j * 2) + 1] = (uint8_t)(sample_bits >> 8);
                }

                uart_send_frame(payload, PCM_FRAME_BYTES);
                frame_pos = 0;
            }
        }
    }
}
