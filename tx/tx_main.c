#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "codec2.h"
#include "audio_clocks.h"
#include "i2s_pio.pio.h"

#define UART_ID uart0
#define BAUD_RATE 115200
#define UART_TX_PIN 0
#define UART_RX_PIN 1

#define I2S_DATA_PIN 8
#define I2S_CLOCK_PIN_BASE 9 // GP9=LRCK, GP10=BCK
#define I2S_SCK_PIN 11

const uint LED_PIN = PICO_DEFAULT_LED_PIN;

queue_t sample_queue;

void core1_i2s_read_task() {
    PIO pio = pio0;
    uint sm = 0;
    
    while (true) {
        // Read 32-bit left channel
        uint32_t left_word = pio_sm_get_blocking(pio, sm);
        // Read 32-bit right channel (ignore for mono)
        uint32_t right_word = pio_sm_get_blocking(pio, sm);
        
        // The I2S spec dictates a 1-clock delay before MSB is sent.
        // Our PIO script samples 1 clock early, placing the MSB at bit 30 instead of 31.
        // We shift left by 1 to properly align the 24-bit ADC data to the MSB of the 32-bit word.
        uint32_t aligned_word = left_word << 1;
        
        // Extract the top 16 bits for Codec 2
        int16_t sample = (int16_t)(aligned_word >> 16);
        
        // Push to queue for Core 0 to process
        queue_try_add(&sample_queue, &sample);
    }
}

int main() {
    stdio_init_all();
    
    // Initialize LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    
    audio_clocks_init(I2S_SCK_PIN);
    
    // Initialize UART
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    
    // Initialize Codec 2
    struct CODEC2 *c2 = codec2_create(CODEC2_MODE_3200);
    int samples_per_frame = codec2_samples_per_frame(c2);
    int bytes_per_frame = codec2_bytes_per_frame(c2);
    
    short *speech = (short *)malloc(samples_per_frame * sizeof(short));
    unsigned char *bits = (unsigned char *)malloc(bytes_per_frame * sizeof(char));
    
    // Initialize Sample Queue (2 frames deep to prevent overflow during encoding)
    queue_init(&sample_queue, sizeof(int16_t), samples_per_frame * 2);
    
    // Initialize PIO for I2S RX
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &audio_i2s_rx_program);
    audio_i2s_rx_program_init(pio, sm, offset, I2S_DATA_PIN, I2S_CLOCK_PIN_BASE);
    
    // Enable PIO
    pio_sm_set_enabled(pio, sm, true);
    
    // Launch Core 1 to handle continuous I2S reading
    multicore_launch_core1(core1_i2s_read_task);
    
    uint32_t frame_counter = 0;
    bool led_state = false;
    const uint8_t sync_word[2] = {0xAA, 0x55};
    
    while (true) {
        // Collect one frame of audio from Core 1 queue
        for (int i = 0; i < samples_per_frame; i++) {
            queue_remove_blocking(&sample_queue, &speech[i]);
        }
        
        // Encode Codec 2 frame
        codec2_encode(c2, bits, speech);
        
        // Transmit sync header and bits over UART
        uart_write_blocking(UART_ID, sync_word, 2);
        uart_write_blocking(UART_ID, bits, bytes_per_frame);
        
        // Toggle LED every ~1 second (8000 Hz / 320 samples = 25 frames per second)
        frame_counter++;
        if (frame_counter >= 25) {
            frame_counter = 0;
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
        }
    }
    
    return 0;
}
