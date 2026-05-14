#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "i2s_rx.pio.h" 
#include "codec2.h" // The Codec 2 library header

// Hardware Pins
#define PIN_MCLK 0
#define PIN_BCK  1
#define PIN_LRCK 2
#define PIN_DOUT 3
#define PIN_UART_TX 4
#define UART_BAUD 115200

// Codec 2 Settings
#define C2_MODE CODEC2_MODE_1300 // 1300 bps is a good balance of quality/compression
#define MAX_SAMPLES_PER_FRAME 320 // Mode 1300 uses 320 samples (40ms of 8kHz audio)
#define MAX_CODEC2_BYTES 16

#define FRAME_SYNC_1 0x55
#define FRAME_SYNC_2 0xD3

// Ping-Pong Buffers for inter-core audio passing
int16_t buffer_A[MAX_SAMPLES_PER_FRAME];
int16_t buffer_B[MAX_SAMPLES_PER_FRAME];
volatile bool filling_buffer_A = true;
volatile bool buffer_A_ready = false;
volatile bool buffer_B_ready = false;

static uint8_t codec2_frame_crc(uint8_t seq, const uint8_t *payload, int payload_len) {
    uint8_t crc = seq;
    for (int i = 0; i < payload_len; i++) {
        crc ^= payload[i];
    }
    return crc;
}

// ---------------------------------------------------------
// CORE 1: The Heavy Lifter (Codec 2 Encoder & UART TX)
// ---------------------------------------------------------
void core1_entry() {
    // 1. Initialize Codec 2
    struct CODEC2 *c2 = codec2_create(C2_MODE);
    int bytes_per_frame = (codec2_bits_per_frame(c2) + 7) / 8; 
    if (bytes_per_frame > MAX_CODEC2_BYTES) {
        while (true) {
            tight_loop_contents();
        }
    }

    uint8_t compressed_bits[MAX_CODEC2_BYTES];
    uint8_t packet[2 + 1 + MAX_CODEC2_BYTES + 1];
    uint8_t frame_seq = 0;

    // 2. Initialize UART for streaming to the second Pico
    uart_init(uart1, UART_BAUD);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart1, false, false);
    gpio_set_function(PIN_UART_TX, GPIO_FUNC_UART);

    while(true) {
        // Wait until a buffer is fully recorded by Core 0
        if (buffer_A_ready) {
            // Encode the 320 samples into roughly 7 bytes!
            codec2_encode(c2, compressed_bits, buffer_A);
            buffer_A_ready = false; // Mark as processed
            
            packet[0] = FRAME_SYNC_1;
            packet[1] = FRAME_SYNC_2;
            packet[2] = frame_seq;
            for (int i = 0; i < bytes_per_frame; i++) {
                packet[3 + i] = compressed_bits[i];
            }
            packet[3 + bytes_per_frame] = codec2_frame_crc(frame_seq, compressed_bits, bytes_per_frame);
            frame_seq++;

            uart_write_blocking(uart1, packet, 4 + bytes_per_frame);
        }
        else if (buffer_B_ready) {
            codec2_encode(c2, compressed_bits, buffer_B);
            buffer_B_ready = false; 

            packet[0] = FRAME_SYNC_1;
            packet[1] = FRAME_SYNC_2;
            packet[2] = frame_seq;
            for (int i = 0; i < bytes_per_frame; i++) {
                packet[3 + i] = compressed_bits[i];
            }
            packet[3 + bytes_per_frame] = codec2_frame_crc(frame_seq, compressed_bits, bytes_per_frame);
            frame_seq++;

            uart_write_blocking(uart1, packet, 4 + bytes_per_frame);
        }
        else {
            tight_loop_contents(); // Yield until Core 0 finishes a buffer
        }
    }
}

// ---------------------------------------------------------
// CORE 0: The Manager (Clocks & I2S Ingestion)
// ---------------------------------------------------------
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
    set_sys_clock_khz(250000, true);

    stdio_init_all();
    setup_mclk();
    
    // Boot up Core 1
    multicore_launch_core1(core1_entry);
    
    // Initialize PIO for I2S
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_slave_rx_program);
    pio_sm_config c = i2s_slave_rx_program_get_default_config(offset);
    sm_config_set_in_pins(&c, PIN_DOUT);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    pio_gpio_init(pio, PIN_DOUT);
    pio_gpio_init(pio, PIN_BCK);
    pio_gpio_init(pio, PIN_LRCK);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_DOUT, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_BCK, 1, false);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_LRCK, 1, false);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    // Discover how many samples Core 1 expects (so we don't hardcode it)
    struct CODEC2 *temp_c2 = codec2_create(C2_MODE);
    int required_samples = codec2_samples_per_frame(temp_c2);
    codec2_destroy(temp_c2);

    int sample_index = 0;

    while(true) {
        // Read Left and Right from PCM1808
        uint32_t left_channel_raw = pio_sm_get_blocking(pio, sm);
        uint32_t right_channel_raw = pio_sm_get_blocking(pio, sm); 
        
        // Extract 16-bit PCM
        int16_t left_audio = (int16_t)(left_channel_raw >> 16);

        // Put the sample in the active buffer
        if (filling_buffer_A) {
            buffer_A[sample_index] = left_audio;
        } else {
            buffer_B[sample_index] = left_audio;
        }

        sample_index++;

        // When the buffer is full, swap to the other buffer and alert Core 1
        if (sample_index >= required_samples) {
            sample_index = 0;
            
            if (filling_buffer_A) {
                buffer_A_ready = true;
                filling_buffer_A = false; // Swap to B
            } else {
                buffer_B_ready = true;
                filling_buffer_A = true;  // Swap to A
            }
        }
    }
}