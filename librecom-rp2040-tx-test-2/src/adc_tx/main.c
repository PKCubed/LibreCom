#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"

#include "adpcm.h"
#include "config.h"
#include "transport.h"
#include "i2s_adc_in.pio.h"

static const uint ADC_PIN_MCLK = 0;
static const uint ADC_PIN_BCK = 1;
static const uint ADC_PIN_LRCK = 2;
static const uint ADC_PIN_DOUT = 3;

static const uint UART_TX_PIN = 4;
static uart_inst_t *const AUDIO_UART = uart1;

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

static void init_uart_tx(void) {
    uart_init(AUDIO_UART, UART_BAUD_RATE);
    uart_set_hw_flow(AUDIO_UART, false, false);
    uart_set_format(AUDIO_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(AUDIO_UART, true);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
}

static void init_i2s_adc_in(PIO pio, uint sm) {
    uint offset = pio_add_program(pio, &i2s_adc_in_program);
    pio_sm_config cfg = i2s_adc_in_program_get_default_config(offset);

    sm_config_set_in_pins(&cfg, ADC_PIN_DOUT);
    sm_config_set_in_shift(&cfg, false, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_RX);

    pio_gpio_init(pio, ADC_PIN_DOUT);
    gpio_set_pulls(ADC_PIN_DOUT, false, false);

    pio_sm_set_consecutive_pindirs(pio, sm, ADC_PIN_DOUT, 1, false);
    pio_sm_init(pio, sm, offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}

static inline int16_t i2s_word_to_pcm16(uint32_t word) {
    return (int16_t)(word >> 20);
}

static int16_t synth_tone_sample(void) {
    static uint16_t phase = 0;
    phase += 3277;
    return (phase & 0x8000u) ? (int16_t)10000 : (int16_t)-10000;
}

int main(void) {
    set_sys_clock_khz(250000, true);
    stdio_init_all();
    sleep_ms(100);

    init_pcm1808_mclk();
    init_uart_tx();

    PIO pio = pio0;
    const uint sm = 0;
    init_i2s_adc_in(pio, sm);

    adpcm_state_t adpcm_state;
    adpcm_state_init(&adpcm_state);

    int16_t pcm_frame[AUDIO_SAMPLES_PER_FRAME];
    uint8_t adpcm_frame[(AUDIO_SAMPLES_PER_FRAME + 1u) / 2u];
    uint8_t packet[TRANSPORT_MAX_PACKET_BYTES];
    uint16_t sequence = 0;

    while (true) {
        for (size_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; ++i) {
#if DEBUG_TX_SYNTH_TONE
            pcm_frame[i] = synth_tone_sample();
#else
            uint32_t left_word = pio_sm_get_blocking(pio, sm);
            (void)pio_sm_get_blocking(pio, sm);
            pcm_frame[i] = i2s_word_to_pcm16(left_word);
#endif
        }

        adpcm_state_t frame_state = adpcm_state;
        adpcm_encode_block(pcm_frame, AUDIO_SAMPLES_PER_FRAME, adpcm_frame, &adpcm_state);

        size_t encoded_len = transport_encode_audio_packet(
            sequence++,
            AUDIO_SAMPLES_PER_FRAME,
            frame_state.predictor,
            frame_state.index,
            adpcm_frame,
            sizeof(adpcm_frame),
            packet,
            sizeof(packet)
        );

        if (encoded_len > 0) {
            uart_write_blocking(AUDIO_UART, packet, encoded_len);
        }
    }
}
