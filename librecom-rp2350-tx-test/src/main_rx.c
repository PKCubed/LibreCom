#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "codec2/codec2.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "i2s_audio.pio.h"

#define UART_DEV uart0
#define UART_RX_PIN 1
#define UART_BAUD_RATE 115200

#define I2S_PIO pio0
#define I2S_SM 0
#define I2S_DATA_PIN 8
#define I2S_LRCK_PIN 9
#define I2S_BCK_PIN 10

#define AUDIO_SAMPLE_RATE 8000
#define I2S_BITS_PER_CHANNEL 32
#define I2S_WORDS_PER_FRAME 2
#define CODEC2_MODE_SELECT CODEC2_MODE_2400

#define PACKET_SOF_1 0xA5
#define PACKET_SOF_2 0x5A

#define MAX_CODEC2_SAMPLES_PER_FRAME 320
#define MAX_CODEC2_BYTES_PER_FRAME 16
#define MAX_PLAYBACK_WORDS (MAX_CODEC2_SAMPLES_PER_FRAME * I2S_WORDS_PER_FRAME)

static CODEC2 *codec2_state;
static int codec2_samples_per_frame_runtime;
static int codec2_bytes_per_frame_runtime;

static int playback_dma_channel;
static volatile bool playback_dma_active;
static volatile int playback_active_slot;
static volatile uint8_t playback_ready_mask;
static uint32_t playback_words[2][MAX_PLAYBACK_WORDS];

static volatile uint8_t uart_packet_ready;
static volatile uint8_t uart_packet_length;
static volatile uint8_t uart_packet_payload[MAX_CODEC2_BYTES_PER_FRAME];

static uint8_t uart_state;
static uint8_t uart_length;
static uint8_t uart_payload_index;
static uint8_t uart_checksum;
static uint8_t uart_frame_buffer[MAX_CODEC2_BYTES_PER_FRAME];

static short codec2_pcm[MAX_CODEC2_SAMPLES_PER_FRAME];

static void __isr dma_irq0_handler(void);
static void __isr uart_irq_handler(void);

static void init_uart(void) {
    uart_init(UART_DEV, UART_BAUD_RATE);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(UART_DEV, 8, 1, UART_PARITY_NONE);
    uart_set_irq_enables(UART_DEV, true, false);
}

static void init_i2s_tx_pio(void) {
    const uint offset = pio_add_program(I2S_PIO, &i2s_tx_program);
    pio_sm_config config = i2s_tx_program_get_default_config(offset);

    sm_config_set_sideset_pins(&config, I2S_LRCK_PIN);
    sm_config_set_out_pins(&config, I2S_DATA_PIN, 1);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

    const float pio_instruction_hz = AUDIO_SAMPLE_RATE * (float)(I2S_BITS_PER_CHANNEL * I2S_WORDS_PER_FRAME) * 2.0f;
    const float clk_sys_hz = (float)clock_get_hz(clk_sys);
    sm_config_set_clkdiv(&config, clk_sys_hz / pio_instruction_hz);

    pio_gpio_init(I2S_PIO, I2S_DATA_PIN);
    pio_gpio_init(I2S_PIO, I2S_LRCK_PIN);
    pio_gpio_init(I2S_PIO, I2S_BCK_PIN);

    pio_sm_set_consecutive_pindirs(I2S_PIO, I2S_SM, I2S_DATA_PIN, 1, true);
    pio_sm_set_consecutive_pindirs(I2S_PIO, I2S_SM, I2S_LRCK_PIN, 2, true);

    pio_sm_init(I2S_PIO, I2S_SM, offset, &config);
    pio_sm_clear_fifos(I2S_PIO, I2S_SM);
    pio_sm_set_enabled(I2S_PIO, I2S_SM, true);
}

static void __isr dma_irq0_handler(void) {
    dma_hw->ints0 = 1u << playback_dma_channel;
    playback_dma_active = false;
    playback_active_slot = -1;
}

static void __isr uart_irq_handler(void) {
    while (uart_is_readable(UART_DEV)) {
        const uint8_t byte = (uint8_t)uart_getc(UART_DEV);

        switch (uart_state) {
        case 0:
            uart_state = (byte == PACKET_SOF_1) ? 1u : 0u;
            break;
        case 1:
            uart_state = (byte == PACKET_SOF_2) ? 2u : 0u;
            break;
        case 2:
            uart_checksum = byte;
            uart_state = 3;
            break;
        case 3:
            uart_length = byte;
            uart_checksum = (uint8_t)(uart_checksum + byte);
            uart_payload_index = 0;
            if (uart_length == codec2_bytes_per_frame_runtime && uart_length <= MAX_CODEC2_BYTES_PER_FRAME) {
                uart_state = 4;
            } else {
                uart_state = 0;
            }
            break;
        case 4:
            uart_frame_buffer[uart_payload_index++] = byte;
            uart_checksum = (uint8_t)(uart_checksum + byte);
            if (uart_payload_index >= uart_length) {
                uart_state = 5;
            }
            break;
        case 5:
            if (uart_checksum == byte && !uart_packet_ready) {
                memcpy((void *)uart_packet_payload, uart_frame_buffer, uart_length);
                uart_packet_length = uart_length;
                uart_packet_ready = 1;
            }
            uart_state = 0;
            break;
        default:
            uart_state = 0;
            break;
        }
    }
}

static void init_playback_dma(void) {
    playback_dma_channel = dma_claim_unused_channel(true);

    dma_channel_config config = dma_channel_get_default_config(playback_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, pio_get_dreq(I2S_PIO, I2S_SM, true));

    dma_channel_configure(
        playback_dma_channel,
        &config,
        &I2S_PIO->txf[I2S_SM],
        playback_words[0],
        (uint32_t)(codec2_samples_per_frame_runtime * I2S_WORDS_PER_FRAME),
        false);

    dma_channel_set_irq0_enabled(playback_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

static void start_playback_dma_if_needed(void) {
    if (playback_dma_active) {
        return;
    }

    const uint8_t ready_mask = playback_ready_mask;
    if (ready_mask == 0) {
        return;
    }

    const int slot = __builtin_ctz((unsigned int)ready_mask);
    playback_ready_mask &= (uint8_t)~(1u << slot);
    playback_active_slot = slot;
    playback_dma_active = true;

    dma_channel_set_read_addr(playback_dma_channel, playback_words[slot], false);
    dma_channel_set_trans_count(playback_dma_channel,
                                (uint32_t)(codec2_samples_per_frame_runtime * I2S_WORDS_PER_FRAME),
                                true);
}

static int reserve_playback_slot(void) {
    if (playback_active_slot >= 0) {
        const int inactive_slot = playback_active_slot ^ 1;
        if (playback_ready_mask & (1u << inactive_slot)) {
            return -1;
        }
        return inactive_slot;
    }

    if (!(playback_ready_mask & 1u)) {
        return 0;
    }
    if (!(playback_ready_mask & 2u)) {
        return 1;
    }
    return -1;
}

static void decode_packet_into_playback_buffer(void) {
    uint8_t packet_length;
    uint8_t packet_payload[MAX_CODEC2_BYTES_PER_FRAME];

    const uint32_t irq_state = save_and_disable_interrupts();
    if (!uart_packet_ready) {
        restore_interrupts(irq_state);
        return;
    }

    packet_length = uart_packet_length;
    memcpy(packet_payload, (const void *)uart_packet_payload, packet_length);
    uart_packet_ready = 0;
    restore_interrupts(irq_state);

    codec2_decode(codec2_state, codec2_pcm, packet_payload);

    const int slot = reserve_playback_slot();
    if (slot < 0) {
        return;
    }

    for (int i = 0; i < codec2_samples_per_frame_runtime; ++i) {
        const uint32_t sample_word = ((uint32_t)((uint16_t)codec2_pcm[i])) << 16;
        playback_words[slot][i * I2S_WORDS_PER_FRAME] = sample_word;
        playback_words[slot][i * I2S_WORDS_PER_FRAME + 1] = sample_word;
    }

    playback_ready_mask |= (uint8_t)(1u << slot);
    start_playback_dma_if_needed();
}

int main(void) {
    stdio_init_all();
    sleep_ms(100);

    init_uart();

    codec2_state = codec2_create(CODEC2_MODE_SELECT);
    codec2_samples_per_frame_runtime = codec2_samples_per_frame(codec2_state);
    codec2_bytes_per_frame_runtime = (codec2_bits_per_frame(codec2_state) + 7) / 8;

    init_i2s_tx_pio();
    init_playback_dma();

    irq_set_exclusive_handler(UART0_IRQ, uart_irq_handler);
    irq_set_enabled(UART0_IRQ, true);

    while (true) {
        decode_packet_into_playback_buffer();
        start_playback_dma_if_needed();
        __wfi();
    }
}