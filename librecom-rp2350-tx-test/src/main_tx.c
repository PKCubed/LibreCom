#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "codec2/codec2.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "i2s_audio.pio.h"

#define UART_DEV uart0
#define UART_TX_PIN 0
#define UART_BAUD_RATE 115200

#define I2S_PIO pio0
#define I2S_SM 0
#define I2S_DATA_PIN 8
#define I2S_LRCK_PIN 9
#define I2S_BCK_PIN 10

#define MCLK_PIN 11
#define AUDIO_SAMPLE_RATE 8000
#define I2S_BITS_PER_CHANNEL 32
#define I2S_WORDS_PER_FRAME 2
#define CODEC2_MODE_SELECT CODEC2_MODE_2400

#define PACKET_SOF_1 0xA5
#define PACKET_SOF_2 0x5A

#define MAX_CODEC2_SAMPLES_PER_FRAME 320
#define MAX_CODEC2_BYTES_PER_FRAME 16
#define MAX_CAPTURE_WORDS (MAX_CODEC2_SAMPLES_PER_FRAME * I2S_WORDS_PER_FRAME)

static CODEC2 *codec2_state;
static int codec2_samples_per_frame_runtime;
static int codec2_bytes_per_frame_runtime;

static int capture_dma_channel;
static volatile uint8_t capture_fill_slot;
static volatile uint8_t capture_ready_mask;
static uint32_t capture_words[2][MAX_CAPTURE_WORDS];

static unsigned char codec2_bits[MAX_CODEC2_BYTES_PER_FRAME];
static short codec2_pcm[MAX_CODEC2_SAMPLES_PER_FRAME];

static void __isr dma_irq0_handler(void);

static void init_uart(void) {
    uart_init(UART_DEV, UART_BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_DEV, 8, 1, UART_PARITY_NONE);
}

static void init_mclk_pwm(void) {
    gpio_set_function(MCLK_PIN, GPIO_FUNC_PWM);

    const uint slice = pwm_gpio_to_slice_num(MCLK_PIN);
    const uint channel = pwm_gpio_to_channel(MCLK_PIN);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 1);

    const float clk_sys_hz = (float)clock_get_hz(clk_sys);
    const float divider = clk_sys_hz / (2048000.0f * 2.0f);
    pwm_config_set_clkdiv(&config, divider);

    pwm_init(slice, &config, false);
    pwm_set_chan_level(slice, channel, 1);
    pwm_set_enabled(slice, true);
}

static void init_i2s_rx_pio(void) {
    const uint offset = pio_add_program(I2S_PIO, &i2s_rx_program);
    pio_sm_config config = i2s_rx_program_get_default_config(offset);

    sm_config_set_sideset_pins(&config, I2S_LRCK_PIN);
    sm_config_set_in_pins(&config, I2S_DATA_PIN);
    sm_config_set_in_shift(&config, true, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);

    const float pio_instruction_hz = AUDIO_SAMPLE_RATE * (float)(I2S_BITS_PER_CHANNEL * I2S_WORDS_PER_FRAME) * 2.0f;
    const float clk_sys_hz = (float)clock_get_hz(clk_sys);
    sm_config_set_clkdiv(&config, clk_sys_hz / pio_instruction_hz);

    pio_gpio_init(I2S_PIO, I2S_DATA_PIN);
    pio_gpio_init(I2S_PIO, I2S_LRCK_PIN);
    pio_gpio_init(I2S_PIO, I2S_BCK_PIN);

    pio_sm_set_consecutive_pindirs(I2S_PIO, I2S_SM, I2S_DATA_PIN, 1, false);
    pio_sm_set_consecutive_pindirs(I2S_PIO, I2S_SM, I2S_LRCK_PIN, 2, true);

    pio_sm_init(I2S_PIO, I2S_SM, offset, &config);
    pio_sm_clear_fifos(I2S_PIO, I2S_SM);
    pio_sm_set_enabled(I2S_PIO, I2S_SM, true);
}

static void start_capture_dma(uint8_t slot) {
    capture_fill_slot = slot;
    dma_channel_set_write_addr(capture_dma_channel, capture_words[slot], false);
    dma_channel_set_trans_count(capture_dma_channel,
                                (uint32_t)(codec2_samples_per_frame_runtime * I2S_WORDS_PER_FRAME),
                                true);
}

static void __isr dma_irq0_handler(void) {
    dma_hw->ints0 = 1u << capture_dma_channel;
    capture_ready_mask |= (uint8_t)(1u << capture_fill_slot);
    start_capture_dma(capture_fill_slot ^ 1u);
}

static void init_capture_dma(void) {
    capture_dma_channel = dma_claim_unused_channel(true);

    dma_channel_config config = dma_channel_get_default_config(capture_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, true);
    channel_config_set_dreq(&config, pio_get_dreq(I2S_PIO, I2S_SM, false));

    dma_channel_configure(
        capture_dma_channel,
        &config,
        capture_words[0],
        &I2S_PIO->rxf[I2S_SM],
        (uint32_t)(codec2_samples_per_frame_runtime * I2S_WORDS_PER_FRAME),
        false);

    dma_channel_set_irq0_enabled(capture_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    start_capture_dma(0);
}

static void send_codec2_frame(const unsigned char *bits, int byte_count, uint8_t sequence) {
    uint8_t header[4];
    header[0] = PACKET_SOF_1;
    header[1] = PACKET_SOF_2;
    header[2] = sequence;
    header[3] = (uint8_t)byte_count;

    uint8_t checksum = 0;
    checksum = (uint8_t)(checksum + header[2]);
    checksum = (uint8_t)(checksum + header[3]);
    for (int i = 0; i < byte_count; ++i) {
        checksum = (uint8_t)(checksum + bits[i]);
    }

    uart_write_blocking(UART_DEV, header, sizeof(header));
    uart_write_blocking(UART_DEV, bits, (size_t)byte_count);
    uart_write_blocking(UART_DEV, &checksum, 1);
}

static void process_capture_slot(uint8_t slot, uint8_t sequence) {
    for (int i = 0; i < codec2_samples_per_frame_runtime; ++i) {
        const uint32_t left_word = capture_words[slot][i * I2S_WORDS_PER_FRAME];
        codec2_pcm[i] = (short)(left_word >> 16);
    }

    codec2_encode(codec2_state, codec2_bits, codec2_pcm);
    send_codec2_frame(codec2_bits, codec2_bytes_per_frame_runtime, sequence);
}

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    init_uart();
    init_mclk_pwm();

    codec2_state = codec2_create(CODEC2_MODE_SELECT);
    codec2_samples_per_frame_runtime = codec2_samples_per_frame(codec2_state);
    codec2_bytes_per_frame_runtime = (codec2_bits_per_frame(codec2_state) + 7) / 8;

    init_i2s_rx_pio();
    init_capture_dma();

    uint8_t sequence = 0;

    while (true) {
        uint8_t ready_mask;
        const uint32_t irq_state = save_and_disable_interrupts();
        ready_mask = capture_ready_mask;
        if (ready_mask != 0) {
            const uint8_t slot = (uint8_t)__builtin_ctz((unsigned int)ready_mask);
            capture_ready_mask &= (uint8_t)~(1u << slot);
            restore_interrupts(irq_state);

            process_capture_slot(slot, sequence++);
            continue;
        }
        restore_interrupts(irq_state);

        __wfi();
    }
}