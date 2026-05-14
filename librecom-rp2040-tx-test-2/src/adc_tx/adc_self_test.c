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

static inline int16_t i2s_word_to_pcm16(uint32_t word) {
    return (int16_t)(word >> 24);
}

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    printf("adc_self_test starting\n");
    printf("Pins: MCLK=%u BCK=%u LRCK=%u DOUT=%u\n", ADC_PIN_MCLK, ADC_PIN_BCK, ADC_PIN_LRCK, ADC_PIN_DOUT);
    printf("Expecting sample rate around %u Hz\n", AUDIO_SAMPLE_RATE_HZ);

    init_pcm1808_mclk();

    PIO pio = pio0;
    const uint sm = 0;
    init_i2s_adc_in(pio, sm);

    const uint32_t report_samples = 800;

    while (true) {
        int16_t left_min = INT16_MAX;
        int16_t left_max = INT16_MIN;
        int16_t right_min = INT16_MAX;
        int16_t right_max = INT16_MIN;

        int32_t left_abs_sum = 0;
        int32_t right_abs_sum = 0;
        int32_t lr_diff_abs_sum = 0;
        uint32_t zero_cross = 0;

        int16_t last_left = 0;
        bool last_valid = false;

        uint32_t collected = 0;
        absolute_time_t start = get_absolute_time();
        absolute_time_t last_data = start;

        while (collected < report_samples) {
            if (pio_sm_is_rx_fifo_empty(pio, sm)) {
                if (absolute_time_diff_us(last_data, get_absolute_time()) > 200000) {
                    printf("NO_DATA: no I2S samples for >200ms (check BCK/LRCK/MCLK/format)\n");
                    last_data = get_absolute_time();
                }
                tight_loop_contents();
                continue;
            }

            uint32_t left_word = pio_sm_get_blocking(pio, sm);
            uint32_t right_word = pio_sm_get_blocking(pio, sm);
            last_data = get_absolute_time();

            int16_t left = i2s_word_to_pcm16(left_word);
            int16_t right = i2s_word_to_pcm16(right_word);

            if (left < left_min) left_min = left;
            if (left > left_max) left_max = left;
            if (right < right_min) right_min = right;
            if (right > right_max) right_max = right;

            left_abs_sum += (left < 0) ? -left : left;
            right_abs_sum += (right < 0) ? -right : right;

            int32_t lr_diff = (int32_t)left - (int32_t)right;
            lr_diff_abs_sum += (lr_diff < 0) ? -lr_diff : lr_diff;

            if (last_valid && ((last_left < 0 && left >= 0) || (last_left >= 0 && left < 0))) {
                zero_cross++;
            }
            last_left = left;
            last_valid = true;
            collected++;
        }

        int32_t left_pp = (int32_t)left_max - (int32_t)left_min;
        int32_t right_pp = (int32_t)right_max - (int32_t)right_min;
        int32_t left_mean_abs = left_abs_sum / (int32_t)report_samples;
        int32_t right_mean_abs = right_abs_sum / (int32_t)report_samples;
        int32_t lr_mean_abs_diff = lr_diff_abs_sum / (int32_t)report_samples;

        int64_t elapsed_us = absolute_time_diff_us(start, get_absolute_time());
        uint32_t measured_hz = elapsed_us > 0 ? (uint32_t)((report_samples * 1000000ull) / (uint64_t)elapsed_us) : 0u;

        const char *health = "OK";
        if (left_pp < 40 && right_pp < 40) {
            health = "VERY_LOW_ACTIVITY";
        } else if (measured_hz < (AUDIO_SAMPLE_RATE_HZ / 2u) || measured_hz > (AUDIO_SAMPLE_RATE_HZ * 2u)) {
            health = "RATE_SUSPECT";
        }

        printf("ADC %s: fs~%lu Hz | L[min=%d max=%d pp=%ld mean|x|=%ld zc=%lu] | R[min=%d max=%d pp=%ld mean|x|=%ld] | LR mean|d|=%ld\n",
               health,
               (unsigned long)measured_hz,
               left_min, left_max, (long)left_pp, (long)left_mean_abs, (unsigned long)zero_cross,
               right_min, right_max, (long)right_pp, (long)right_mean_abs,
               (long)lr_mean_abs_diff);
    }
}
