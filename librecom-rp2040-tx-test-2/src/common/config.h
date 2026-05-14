#pragma once

#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ         8000u
#define AUDIO_FRAME_MS               20u
#define AUDIO_SAMPLES_PER_FRAME      ((AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_MS) / 1000u)

#define I2S_BITS_PER_CHANNEL         32u

#define ADC_MCLK_TARGET_HZ           2048000u

#define UART_BAUD_RATE               115200u

#define TRANSPORT_PACKET_TYPE_AUDIO  0xA1u
#define TRANSPORT_MAX_ADPCM_BYTES    128u
#define TRANSPORT_MAX_PACKET_BYTES   256u

#define DEBUG_TX_SYNTH_TONE          0u
#define DEBUG_RX_SYNTH_TONE          0u
