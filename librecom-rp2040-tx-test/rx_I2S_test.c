#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "i2s_tx.pio.h" // Your existing generated PIO header

// RX Hardware Pins
#define PIN_DAC_DATA 6
#define PIN_DAC_BCLK 7 
#define PIN_DAC_LRCK 8 

// Audio parameters matching your Codec 2 setup
#define SAMPLE_RATE 8000
#define SINE_FREQ 400
#define SAMPLES_PER_CYCLE (SAMPLE_RATE / SINE_FREQ) // 20 samples per wave

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int16_t sine_table[SAMPLES_PER_CYCLE];

int main() {
    set_sys_clock_khz(250000, true); 
    stdio_init_all();

    // 1. Pre-compute the sine wave into a lookup table
    // We set the amplitude to 16000 (about 50% max volume) so it doesn't deafen you
    for (int i = 0; i < SAMPLES_PER_CYCLE; i++) {
        sine_table[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * i / SAMPLES_PER_CYCLE));
    }

    // 2. Initialize the I2S PIO
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_tx_program);
    
    pio_sm_config c = i2s_tx_program_get_default_config(offset);
    sm_config_set_out_pins(&c, PIN_DAC_DATA, 1);
    sm_config_set_sideset_pins(&c, PIN_DAC_BCLK);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    
    // 8kHz, 64 bits per frame, 2 PIO cycles per bit
    float div = (float)clock_get_hz(clk_sys) / (SAMPLE_RATE * 64.0f * 2.0f); 
    sm_config_set_clkdiv(&c, div);

    pio_gpio_init(pio, PIN_DAC_DATA);
    pio_gpio_init(pio, PIN_DAC_BCLK);
    pio_gpio_init(pio, PIN_DAC_LRCK);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_DAC_DATA, 3, true);
    
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    int sample_idx = 0;

    // 3. Blast the sine wave continuously
    while(true) {
        // Grab the 16-bit sine sample and shift it to the MSB of the 32-bit register
        uint32_t sample = ((uint32_t)(uint16_t)sine_table[sample_idx]) << 16;
        
        // Push to PIO (Left and Right channels)
        pio_sm_put_blocking(pio, sm, sample); 
        pio_sm_put_blocking(pio, sm, sample); 
        
        sample_idx++;
        if (sample_idx >= SAMPLES_PER_CYCLE) {
            sample_idx = 0; // Wrap around to the start of the sine wave
        }
    }
}