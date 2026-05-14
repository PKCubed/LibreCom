#include <math.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "i2s_dac_out.pio.h"

#define PIN_DAC_DATA 6
#define PIN_DAC_BCLK 7
#define PIN_DAC_LRCK 8

#define SAMPLE_RATE 8000
#define SINE_FREQ 400
#define SAMPLES_PER_CYCLE (SAMPLE_RATE / SINE_FREQ)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t sine_table[SAMPLES_PER_CYCLE];

int main(void) {
    stdio_init_all();

    for (int i = 0; i < SAMPLES_PER_CYCLE; ++i) {
        sine_table[i] = (int16_t)(16000.0f * sinf(2.0f * (float)M_PI * (float)i / (float)SAMPLES_PER_CYCLE));
    }

    PIO pio = pio0;
    const uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_dac_out_program);

    pio_sm_config cfg = i2s_dac_out_program_get_default_config(offset);
    sm_config_set_out_pins(&cfg, PIN_DAC_DATA, 1);
    sm_config_set_sideset_pins(&cfg, PIN_DAC_BCLK);
    sm_config_set_out_shift(&cfg, false, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);

    float div = (float)clock_get_hz(clk_sys) / (SAMPLE_RATE * 64.0f * 2.0f);
    sm_config_set_clkdiv(&cfg, div);

    pio_gpio_init(pio, PIN_DAC_DATA);
    pio_gpio_init(pio, PIN_DAC_BCLK);
    pio_gpio_init(pio, PIN_DAC_LRCK);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_DAC_DATA, 3, true);

    pio_sm_init(pio, sm, offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);

    int sample_idx = 0;

    while (true) {
        uint32_t sample = ((uint32_t)(uint16_t)sine_table[sample_idx]) << 16;
        pio_sm_put_blocking(pio, sm, sample);
        pio_sm_put_blocking(pio, sm, sample);

        ++sample_idx;
        if (sample_idx >= SAMPLES_PER_CYCLE) {
            sample_idx = 0;
        }
    }
}
