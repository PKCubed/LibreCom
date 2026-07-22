#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "tx";

#define I2S_MCK_IO      (16)
#define I2S_BCK_IO      (17)
#define I2S_WS_IO       (18)
#define I2S_DO_IO       (19) // PCM1808 DOUT -> ESP32-S3 DIN
#define I2S_DI_IO       (-1) 

#define UART_TX_IO      (4)
#define UART_RX_IO      (5)

#define SAMPLE_RATE     (8000)
#define UART_NUM        (UART_NUM_1)
#define UART_BAUD       (460800)
#define BUF_SIZE        (1024)

i2s_chan_handle_t rx_chan;

void init_i2s(void)
{
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DI_IO, // No output from ESP
            .din  = I2S_DO_IO, // Input to ESP
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    rx_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}

void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX_IO, UART_RX_IO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Transmitter");
    init_uart();
    init_i2s();

    uint8_t *rx_buf = calloc(1, BUF_SIZE);
    size_t r_bytes = 0;

    ESP_LOGI(TAG, "Starting Audio Transmission (Sine Wave Test)");
    
    int16_t *samples = calloc(BUF_SIZE / sizeof(int16_t), sizeof(int16_t));
    float phase = 0.0f;
    float phase_inc = 2.0f * 3.14159265359f * 440.0f / (float)SAMPLE_RATE;

    while (1) {
        for (int i = 0; i < (BUF_SIZE / sizeof(int16_t)); i++) {
            samples[i] = (int16_t)(16000.0f * sinf(phase));
            phase += phase_inc;
            if (phase >= 2.0f * 3.14159265359f) {
                phase -= 2.0f * 3.14159265359f;
            }
        }
        
        // Send the sine wave block over UART instead of I2S data
        uart_write_bytes(UART_NUM, (const char *)samples, BUF_SIZE);
        
        // 1024 bytes (512 samples) at 8000Hz takes exactly 64 milliseconds to play. 
        // We MUST delay here to prevent overflowing the receiver's UART buffer!
        vTaskDelay(pdMS_TO_TICKS(64)); 
    }
}
