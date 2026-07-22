#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "rx";

#define I2S_BCK_IO      (17)
#define I2S_WS_IO       (18)
#define I2S_DO_IO       (19) // ESP32-S3 DOUT -> PCM5102A DIN
#define I2S_DI_IO       (-1)
#define I2S_MCK_IO      (-1) // Not needed, PCM5102A uses internal PLL

#define UART_TX_IO      (4)
#define UART_RX_IO      (5)

#define SAMPLE_RATE     (8000)
#define UART_NUM        (UART_NUM_1)
#define UART_BAUD       (460800)
#define BUF_SIZE        (1024)

i2s_chan_handle_t tx_chan;

void init_i2s(void)
{
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din  = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
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
    ESP_LOGI(TAG, "Initializing Receiver");
    init_uart();
    init_i2s();

    uint8_t *rx_buf = calloc(1, BUF_SIZE);
    size_t w_bytes = 0;

    ESP_LOGI(TAG, "Starting Audio Reception");
    int accumulated = 0;
    while (1) {
        // Read into the buffer at the offset of whatever we've already accumulated
        int rxBytes = uart_read_bytes(UART_NUM, rx_buf + accumulated, BUF_SIZE - accumulated, portMAX_DELAY);
        if (rxBytes > 0) {
            accumulated += rxBytes;
            
            // Only write to I2S when we have a full, perfectly aligned chunk
            if (accumulated == BUF_SIZE) {
                i2s_channel_write(tx_chan, rx_buf, BUF_SIZE, &w_bytes, portMAX_DELAY);
                accumulated = 0; // Reset for the next chunk
            }
        }
    }
}
