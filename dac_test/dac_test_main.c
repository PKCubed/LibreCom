#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "audio_clocks.h"
#include "i2s_pio.pio.h"

#define I2S_DATA_PIN 8
#define I2S_CLOCK_PIN_BASE 9 // GP9=LRCK, GP10=BCK
#define I2S_SCK_PIN 11
#define LED_PIN 25

#define SAMPLE_RATE 8000
#define SINE_FREQ 440.0f
#define PI 3.14159265358979323846f

int main() {
    stdio_init_all();
    
    // Initialize LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    
    audio_clocks_init(I2S_SCK_PIN);
    
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &audio_i2s_tx_program);
    audio_i2s_tx_program_init(pio, sm, offset, I2S_DATA_PIN, I2S_CLOCK_PIN_BASE);
    
    pio_sm_set_enabled(pio, sm, true);
    
    float phase = 0.0f;
    float phase_inc = 2.0f * PI * SINE_FREQ / SAMPLE_RATE;
    
    uint32_t counter = 0;
    bool led_state = false;
    
    while (true) {
        float sine_val = sinf(phase);
        int16_t sample = (int16_t)(sine_val * 16383.0f);
        uint32_t i2s_word = ((uint32_t)(uint16_t)sample << 16) | (uint16_t)sample;
        
        pio_sm_put_blocking(pio, sm, i2s_word);
        
        phase += phase_inc;
        if (phase >= 2.0f * PI) {
            phase -= 2.0f * PI;
        }
        
        // Debug output and LED blink every 8000 samples (1 second)
        counter++;
        if (counter >= 8000) {
            counter = 0;
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            printf("DAC Test Running! System Clock: %lu Hz (Targeting 150 MHz default)\n", 
                   clock_get_hz(clk_sys));
        }
    }
    
    return 0;
}
