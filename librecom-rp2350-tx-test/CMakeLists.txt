cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Target board")

if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Point it at your Pico SDK checkout.")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(librecom_rp2350_audio_link C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(CODEC2_ROOT "" CACHE PATH "Path to a Codec2 install or build tree")

find_path(CODEC2_INCLUDE_DIR
    NAMES codec2/codec2.h codec2.h
    HINTS
        ${CODEC2_ROOT}/include
        ${CODEC2_ROOT}
        ENV CODEC2_ROOT
        ENV CODEC2_PATH
)

find_library(CODEC2_LIBRARY
    NAMES codec2 libcodec2
    HINTS
        ${CODEC2_ROOT}/lib
        ${CODEC2_ROOT}/build
        ${CODEC2_ROOT}
        ENV CODEC2_ROOT
        ENV CODEC2_PATH
)

if(NOT CODEC2_INCLUDE_DIR OR NOT CODEC2_LIBRARY)
    message(FATAL_ERROR "Codec2 not found. Set CODEC2_ROOT to a Codec2 install or build tree.")
endif()

set(AUDIO_COMMON_LIBS
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_uart
    hardware_pwm
    hardware_clocks
    ${CODEC2_LIBRARY}
)

add_executable(tx_firmware
    src/main_tx.c
)

pico_generate_pio_header(tx_firmware ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_audio.pio)
target_include_directories(tx_firmware PRIVATE ${CODEC2_INCLUDE_DIR})
target_link_libraries(tx_firmware PRIVATE ${AUDIO_COMMON_LIBS})
pico_enable_stdio_usb(tx_firmware 0)
pico_enable_stdio_uart(tx_firmware 0)
pico_add_extra_outputs(tx_firmware)

add_executable(rx_firmware
    src/main_rx.c
)

pico_generate_pio_header(rx_firmware ${CMAKE_CURRENT_LIST_DIR}/pio/i2s_audio.pio)
target_include_directories(rx_firmware PRIVATE ${CODEC2_INCLUDE_DIR})
target_link_libraries(rx_firmware PRIVATE ${AUDIO_COMMON_LIBS})
pico_enable_stdio_usb(rx_firmware 0)
pico_enable_stdio_uart(rx_firmware 0)
pico_add_extra_outputs(rx_firmware)