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

void core1_i2s_write_task() {
    PIO pio = pio0;
    uint sm = 0;
    
    while (true) {
        int16_t sample = 0;
        // If queue is empty (due to encoding delays or UART loss), we just output 0.
        // This ensures the PIO NEVER stalls, keeping the I2S clocks perfectly continuous!
        queue_try_remove(&sample_queue, &sample);
        
        // Format sample for 32-bit I2S (MSB shifted to top 16 bits)
        uint32_t i2s_word = ((uint32_t)(uint16_t)sample) << 16;
        
        // Write Left channel 32-bit word
        pio_sm_put_blocking(pio, sm, i2s_word);
        // Write Right channel 32-bit word
        pio_sm_put_blocking(pio, sm, i2s_word);
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
    
    // Initialize Sample Queue (buffer up to 2 frames)
    queue_init(&sample_queue, sizeof(int16_t), samples_per_frame * 2);
    
    // Initialize PIO for I2S TX
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &audio_i2s_tx_program);
    audio_i2s_tx_program_init(pio, sm, offset, I2S_DATA_PIN, I2S_CLOCK_PIN_BASE);
    
    // Enable PIO
    pio_sm_set_enabled(pio, sm, true);
    
    // Launch Core 1 to handle continuous I2S writing
    multicore_launch_core1(core1_i2s_write_task);
    
    uint32_t frame_counter = 0;
    bool led_state = false;
    
    while (true) {
        // Sync to UART header (0xAA, 0x55) to prevent byte misalignment
        uint8_t sync_byte;
        do {
            uart_read_blocking(UART_ID, &sync_byte, 1);
        } while (sync_byte != 0xAA);
        
        uart_read_blocking(UART_ID, &sync_byte, 1);
        if (sync_byte != 0x55) {
            continue; // False positive, resync
        }
        
        // Read the Codec 2 payload
        uart_read_blocking(UART_ID, bits, bytes_per_frame);
        
        // Decode the frame
        codec2_decode(c2, speech, bits);
        
        // Push the decoded samples to the Core 1 queue
        for (int i = 0; i < samples_per_frame; i++) {
            queue_add_blocking(&sample_queue, &speech[i]);
        }
        
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
