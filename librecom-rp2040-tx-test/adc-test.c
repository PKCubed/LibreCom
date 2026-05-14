#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "i2s_rx.pio.h" 

// Updated for Waveshare RP2040-Zero
#define PIN_MCLK 0
#define PIN_BCK  1
#define PIN_LRCK 2
#define PIN_DOUT 3

void setup_mclk() {
    gpio_set_function(PIN_MCLK, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(PIN_MCLK);
    
    float div = (float)clock_get_hz(clk_sys) / (2048000.0f * 2.0f);
    
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, div);
    pwm_config_set_wrap(&config, 1);
    pwm_init(slice_num, &config, true);
    
    pwm_set_gpio_level(PIN_MCLK, 1); 
}

int main() {
    stdio_init_all();
    setup_mclk();
    
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_slave_rx_program);
    
    pio_sm_config c = i2s_slave_rx_program_get_default_config(offset);
    
    sm_config_set_in_pins(&c, PIN_DOUT);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    
    printf("Audio system initialized. Reading 8kHz PCM...\n");

    while(true) {
        uint32_t left_channel_raw = pio_sm_get_blocking(pio, sm);
        uint32_t right_channel_raw = pio_sm_get_blocking(pio, sm); 
        
        int16_t left_audio = (int16_t)(left_channel_raw >> 16);
        
        printf("%d\n", left_audio);
    }
}