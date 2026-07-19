#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "audio_clocks.h"
#include "i2s_pio.pio.h"

#define UART_ID uart0
#define BAUD_RATE 1000000 // 1 Mbps for raw audio
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
        queue_try_remove(&sample_queue, &sample); // output 0 if queue empty
        
        uint32_t i2s_word = ((uint32_t)(uint16_t)sample) << 16;
        pio_sm_put_blocking(pio, sm, i2s_word);
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
    
    // Initialize Sample Queue
    queue_init(&sample_queue, sizeof(int16_t), 128);
    
    // Initialize PIO for I2S TX
    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &audio_i2s_tx_program);
    audio_i2s_tx_program_init(pio, sm, offset, I2S_DATA_PIN, I2S_CLOCK_PIN_BASE);
    
    pio_sm_set_enabled(pio, sm, true);
    multicore_launch_core1(core1_i2s_write_task);
    
    uint32_t counter = 0;
    bool led_state = false;
    
    while (true) {
        // Sync to UART header (0xAA, 0x55)
        uint8_t sync_byte;
        do {
            uart_read_blocking(UART_ID, &sync_byte, 1);
        } while (sync_byte != 0xAA);
        
        uart_read_blocking(UART_ID, &sync_byte, 1);
        if (sync_byte != 0x55) {
            continue;
        }
        
        int16_t sample;
        uart_read_blocking(UART_ID, (uint8_t *)&sample, sizeof(sample));
        
        queue_add_blocking(&sample_queue, &sample);
        
        counter++;
        if (counter >= 8000) {
            counter = 0;
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
        }
    }
    
    return 0;
}
