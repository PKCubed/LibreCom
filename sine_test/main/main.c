#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "sine_test";

#define I2S_BCK_IO      (17)
#define I2S_WS_IO       (18)
#define I2S_DO_IO       (19) // ESP32-S3 DOUT -> PCM5102A DIN
#define I2S_DI_IO       (-1)
#define I2S_MCK_IO      (-1) // Not needed, PCM5102A uses internal PLL

#define SAMPLE_RATE     (8000)
#define SINE_FREQ       (440.0f) // 440 Hz (A4 note)
#define BUF_SAMPLES     (512)

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

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing DAC Sine Test");
    init_i2s();

    int16_t *samples = calloc(BUF_SAMPLES, sizeof(int16_t));
    float phase = 0.0f;
    float phase_inc = 2.0f * (float)M_PI * SINE_FREQ / (float)SAMPLE_RATE;

    ESP_LOGI(TAG, "Playing 440Hz Tone...");
    while (1) {
        // Generate a block of sine wave samples
        for (int i = 0; i < BUF_SAMPLES; i++) {
            samples[i] = (int16_t)(16000.0f * sinf(phase));
            phase += phase_inc;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }

        size_t w_bytes = 0;
        i2s_channel_write(tx_chan, samples, BUF_SAMPLES * sizeof(int16_t), &w_bytes, portMAX_DELAY);
    }
}
