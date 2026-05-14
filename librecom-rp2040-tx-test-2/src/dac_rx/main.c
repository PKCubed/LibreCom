#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/uart.h"

#include "adpcm.h"
#include "config.h"
#include "ringbuf.h"
#include "transport.h"
#include "i2s_dac_out.pio.h"

static const uint DAC_PIN_DIN = 6;
static const uint DAC_PIN_BCK = 7;
static const uint DAC_PIN_LRCK = 8;

static const uint UART_RX_PIN = 5;
static uart_inst_t *const AUDIO_UART = uart1;

static void init_uart_rx(void) {
    uart_init(AUDIO_UART, UART_BAUD_RATE);
    uart_set_hw_flow(AUDIO_UART, false, false);
    uart_set_format(AUDIO_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(AUDIO_UART, true);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
}

static void init_i2s_dac_out(PIO pio, uint sm) {
    uint offset = pio_add_program(pio, &i2s_dac_out_program);
    pio_sm_config cfg = i2s_dac_out_program_get_default_config(offset);

    sm_config_set_out_pins(&cfg, DAC_PIN_DIN, 1);
    sm_config_set_sideset_pins(&cfg, DAC_PIN_BCK);
    sm_config_set_out_shift(&cfg, false, true, 32);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);

    float sm_clk = (float)(AUDIO_SAMPLE_RATE_HZ * 64u * 2u);
    float clkdiv = (float)clock_get_hz(clk_sys) / sm_clk;
    sm_config_set_clkdiv(&cfg, clkdiv);

    pio_gpio_init(pio, DAC_PIN_DIN);
    pio_gpio_init(pio, DAC_PIN_BCK);
    pio_gpio_init(pio, DAC_PIN_LRCK);

    pio_sm_set_consecutive_pindirs(pio, sm, DAC_PIN_DIN, 3, true);

    pio_sm_init(pio, sm, offset, &cfg);
    pio_sm_set_enabled(pio, sm, true);
}

static inline uint32_t pcm16_to_i2s_word(int16_t sample) {
    return ((uint32_t)(uint16_t)sample) << 16;
}

static int16_t synth_tone_sample(void) {
    static uint16_t phase = 0;
    phase += 1638;
    int32_t tri = (phase & 0x8000u) ? (32767 - (int32_t)(phase & 0x7FFFu)) : (int32_t)(phase & 0x7FFFu);
    int32_t centered = (tri - 16384) * 2;
    return (int16_t)(centered / 2);
}

int main(void) {
    set_sys_clock_khz(250000, true);
    stdio_init_all();
    sleep_ms(100);

    init_uart_rx();

    PIO pio = pio0;
    const uint sm = 0;
    init_i2s_dac_out(pio, sm);

    adpcm_state_t adpcm_state;
    adpcm_state_init(&adpcm_state);

    int16_t sample_storage[4096];
    sample_ringbuf_t sample_rb;
    sample_ringbuf_init(&sample_rb, sample_storage, 4096);

    uint8_t encoded_packet[TRANSPORT_MAX_PACKET_BYTES];
    size_t encoded_packet_len = 0;

    uint8_t adpcm_frame[TRANSPORT_MAX_ADPCM_BYTES];
    int16_t decoded_frame[AUDIO_SAMPLES_PER_FRAME];

    uint16_t expected_seq = 0;
    int16_t held_sample = 0;
    bool send_right = false;

    while (true) {
        while (uart_is_readable(AUDIO_UART)) {
            uint8_t byte = (uint8_t)uart_getc(AUDIO_UART);
            if (byte == 0u) {
                if (encoded_packet_len > 0u) {
                    uint16_t sequence = 0;
                    uint16_t sample_count = 0;
                    int16_t predictor = 0;
                    int8_t index = 0;
                    size_t adpcm_len = sizeof(adpcm_frame);

                    bool ok = transport_decode_audio_packet(
                        encoded_packet,
                        encoded_packet_len,
                        &sequence,
                        &sample_count,
                        &predictor,
                        &index,
                        adpcm_frame,
                        &adpcm_len
                    );

                    if (ok && sample_count <= AUDIO_SAMPLES_PER_FRAME) {
                        if (sequence != expected_seq) {
                            expected_seq = sequence;
                        }
                        expected_seq++;

                        adpcm_state.predictor = predictor;
                        adpcm_state.index = index;
                        adpcm_decode_block(adpcm_frame, sample_count, decoded_frame, &adpcm_state);
                        for (size_t i = 0; i < sample_count; ++i) {
                            sample_ringbuf_push(&sample_rb, decoded_frame[i]);
                        }
                    }
                }
                encoded_packet_len = 0;
            } else if (encoded_packet_len < sizeof(encoded_packet)) {
                encoded_packet[encoded_packet_len++] = byte;
            } else {
                encoded_packet_len = 0;
            }
        }

        while (!pio_sm_is_tx_fifo_full(pio, sm)) {
            if (!send_right) {
#if DEBUG_RX_SYNTH_TONE
                held_sample = synth_tone_sample();
#else
                int16_t next_sample;
                if (sample_ringbuf_pop(&sample_rb, &next_sample)) {
                    held_sample = next_sample;
                }
#endif
                pio_sm_put(pio, sm, pcm16_to_i2s_word(held_sample));
                send_right = true;
            } else {
                pio_sm_put(pio, sm, pcm16_to_i2s_word(held_sample));
                send_right = false;
            }
        }
    }
}
