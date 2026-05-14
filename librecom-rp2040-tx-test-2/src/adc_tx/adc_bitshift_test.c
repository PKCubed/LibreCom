#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "config.h"
#include "i2s_adc_in.pio.h"

static const uint ADC_PIN_MCLK = 0;
static const uint ADC_PIN_BCK = 1;
static const uint ADC_PIN_LRCK = 2;
static const uint ADC_PIN_DOUT = 3;

static void init_pcm1808_mclk(void) {
    gpio_set_function(ADC_PIN_MCLK, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(ADC_PIN_MCLK);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 1);
    float clkdiv = (float)clock_get_hz(clk_sys) / (2.0f * (float)ADC_MCLK_TARGET_HZ);
    pwm_config_set_clkdiv(&cfg, clkdiv);
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level(ADC_PIN_MCLK, 1);
}

static void init_i2s_adc_in(PIO pio, uint sm) {
    uint offset = pio_add_program(pio, &i2s_adc_in_program);
    pio_sm_config cfg = i2s_adc_in_program_get_default_config(offset);

    sm_config_set_in_pins(&cfg, ADC_PIN_DOUT);
    sm_config_set_in_shift(&cfg, false, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);

    pio_gpio_init(pio, ADC_PIN_DOUT);
    pio_gpio_init(pio, ADC_PIN_BCK);
    pio_gpio_init(pio, ADC_PIN_LRCK);
    gpio_set_pulls(ADC_PIN_DOUT, false, false);

    pio_sm_set_consecutive_pindirs(pio, sm, ADC_PIN_DOUT, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, ADC_PIN_BCK, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, ADC_PIN_LRCK, 1, false);

    pio_sm_init(pio, sm, offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}

static int16_t extract_with_shift(uint32_t word, int shift_bits) {
    if (shift_bits >= 0) {
        return (int16_t)(word >> (16 + shift_bits));
    } else {
        return (int16_t)((word << (-shift_bits)) >> 16);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    printf("=== ADC BIT-SHIFT ANALYZER ===\n");
    printf("Connect LEFT to GND, leave RIGHT floating for clean alignment test.\n\n");

    init_pcm1808_mclk();

    PIO pio = pio0;
    const uint sm = 0;
    init_i2s_adc_in(pio, sm);

    sleep_ms(500);

    const uint32_t num_samples = 400;

    while (true) {
        uint32_t raw_words_L[num_samples];
        uint32_t raw_words_R[num_samples];

        for (uint32_t i = 0; i < num_samples; i++) {
            raw_words_L[i] = pio_sm_get_blocking(pio, sm);
            raw_words_R[i] = pio_sm_get_blocking(pio, sm);
        }

        printf("Testing shifts from -8 to +8:\n");
        for (int shift = -8; shift <= 8; shift++) {
            int32_t left_abs_sum = 0;
            int32_t right_abs_sum = 0;

            for (uint32_t i = 0; i < num_samples; i++) {
                int16_t left = extract_with_shift(raw_words_L[i], shift);
                int16_t right = extract_with_shift(raw_words_R[i], shift);

                left_abs_sum += (left < 0) ? -left : left;
                right_abs_sum += (right < 0) ? -right : right;
            }

            int32_t left_mean = left_abs_sum / (int32_t)num_samples;
            int32_t right_mean = right_abs_sum / (int32_t)num_samples;

            printf("  shift %+3d: L mean|x|=%5ld | R mean|x|=%5ld", shift, (long)left_mean, (long)right_mean);

            if (left_mean < 1000 && right_mean < 1000) {
                printf(" <-- GOOD (LEFT grounded)");
            }
            printf("\n");
        }

        printf("\n");
        sleep_ms(2000);
    }
}
