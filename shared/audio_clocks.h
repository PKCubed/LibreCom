#ifndef AUDIO_CLOCKS_H
#define AUDIO_CLOCKS_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

// Uses default 150 MHz system clock (RP2350 default).
// Generates a synchronous SCK using PWM.
static inline void audio_clocks_init(uint sck_pin) {
    // We use the default 150 MHz system clock.
    // PWM period = 37 cycles (wrap = 36).
    // SCK = 150 MHz / 37 = 4.054 MHz (512 fS).
    // PIO BCK = 150 MHz / (148 * 2) = 506.756 kHz.
    // SCK / BCK = exactly 8.0!
    // LRCK = 7918 Hz (1.0% error from 8000 Hz, totally fine for Codec 2).
    
    // Configure PWM on the SCK pin
    gpio_set_function(sck_pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(sck_pin);
    
    // Divider is 1.0
    pwm_set_clkdiv(slice_num, 1.0f);
    
    // Wrap at 36 to get 37 cycles per PWM period
    pwm_set_wrap(slice_num, 36);
    
    // 50% duty cycle (18 is half of 36)
    pwm_set_chan_level(slice_num, pwm_gpio_to_channel(sck_pin), 18);
    
    // Enable the PWM
    pwm_set_enabled(slice_num, true);
}

#endif
