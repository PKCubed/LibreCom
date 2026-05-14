#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "i2s_audio.pio.h"

#define I2S_DATA_PIN 8
#define I2S_LRCK_PIN 9
#define I2S_BCK_PIN 10

#define SAMPLE_RATE_HZ 48000
#define I2S_BITS_PER_CHANNEL 32
#define I2S_CHANNELS 2
#define I2S_BCK_HZ (SAMPLE_RATE_HZ * I2S_BITS_PER_CHANNEL * I2S_CHANNELS)

#define DMA_MONO_SAMPLES_PER_BLOCK 128
#define DMA_WORDS_PER_BLOCK (DMA_MONO_SAMPLES_PER_BLOCK * 2)

#define SINE_TONE_HZ 1000.0f
#define SINE_AMPLITUDE 12000.0f

static PIO pio_inst = pio0;
static const uint sm_bck = 0;
static const uint sm_lrck = 1;
static const uint sm_data = 2;

static int dma_chan_tx = -1;
static volatile uint32_t dma_tx_buffers[2][DMA_WORDS_PER_BLOCK];
static volatile bool tx_buffer_needs_fill[2];
static volatile uint8_t dma_active_read_idx = 0;

static int32_t phase_q16 = 0;
static int32_t phase_step_q16 = 0;

static float calc_div_for_square_wave(uint32_t target_hz) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    return (float)sys_hz / (2.0f * (float)target_hz);
}

static int16_t next_sine_sample(void) {
    const float phase = (float)phase_q16 * (2.0f * (float)M_PI / 65536.0f);
    const int16_t s = (int16_t)(sinf(phase) * SINE_AMPLITUDE);

    phase_q16 += phase_step_q16;
    phase_q16 &= 0xFFFF;
    return s;
}

static void fill_dma_tx_buffer(uint8_t idx) {
    volatile uint32_t *dst = dma_tx_buffers[idx];

    for (int i = 0; i < DMA_MONO_SAMPLES_PER_BLOCK; ++i) {
        const int16_t s = next_sine_sample();
        const uint32_t word = ((uint32_t)(uint16_t)s) << 16;

        dst[2 * i] = word;
        dst[(2 * i) + 1] = word;
    }
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

int main(void) {
    stdio_init_all();

    memset((void *)tx_buffer_needs_fill, 0, sizeof(tx_buffer_needs_fill));

    phase_step_q16 = (int32_t)((SINE_TONE_HZ * 65536.0f) / (float)SAMPLE_RATE_HZ);

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
