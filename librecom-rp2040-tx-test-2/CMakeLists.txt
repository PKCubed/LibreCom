cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Point it to your pico-sdk checkout.")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(librecom_speech_link C CXX ASM)
pico_sdk_init()

add_library(speech_common STATIC
    src/common/adpcm.c
    src/common/transport.c
    src/common/ringbuf.c
)

target_include_directories(speech_common PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src/common
)

target_link_libraries(speech_common PUBLIC
    pico_stdlib
)

add_executable(adc_tx
    src/adc_tx/main.c
)

pico_generate_pio_header(adc_tx ${CMAKE_CURRENT_LIST_DIR}/src/pio/i2s_adc_in.pio)

target_link_libraries(adc_tx PRIVATE
    speech_common
    pico_stdlib
    hardware_pio
    hardware_pwm
    hardware_uart
    hardware_clocks
)

pico_enable_stdio_usb(adc_tx 0)
pico_enable_stdio_uart(adc_tx 0)
pico_add_extra_outputs(adc_tx)

add_executable(adc_self_test
    src/adc_tx/adc_self_test.c
)

pico_generate_pio_header(adc_self_test ${CMAKE_CURRENT_LIST_DIR}/src/pio/i2s_adc_in.pio)

target_link_libraries(adc_self_test PRIVATE
    pico_stdlib
    hardware_pio
    hardware_pwm
    hardware_clocks
)

target_include_directories(adc_self_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/common
)

pico_enable_stdio_usb(adc_self_test 1)
pico_enable_stdio_uart(adc_self_test 0)
pico_add_extra_outputs(adc_self_test)

add_executable(adc_bitshift_test
    src/adc_tx/adc_bitshift_test.c
)

pico_generate_pio_header(adc_bitshift_test ${CMAKE_CURRENT_LIST_DIR}/src/pio/i2s_adc_in.pio)

target_link_libraries(adc_bitshift_test PRIVATE
    pico_stdlib
    hardware_pio
    hardware_pwm
    hardware_clocks
)

target_include_directories(adc_bitshift_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/common
)

pico_enable_stdio_usb(adc_bitshift_test 1)
pico_enable_stdio_uart(adc_bitshift_test 0)
pico_add_extra_outputs(adc_bitshift_test)

add_executable(dac_rx
    src/dac_rx/main.c
)

pico_generate_pio_header(dac_rx ${CMAKE_CURRENT_LIST_DIR}/src/pio/i2s_dac_out.pio)

target_link_libraries(dac_rx PRIVATE
    speech_common
    pico_stdlib
    hardware_pio
    hardware_uart
    hardware_clocks
)

pico_enable_stdio_usb(dac_rx 0)
pico_enable_stdio_uart(dac_rx 0)
pico_add_extra_outputs(dac_rx)

add_executable(dac_wave_test
    src/dac_rx/dac_wave_test.c
)

pico_generate_pio_header(dac_wave_test ${CMAKE_CURRENT_LIST_DIR}/src/pio/i2s_dac_out.pio)

target_link_libraries(dac_wave_test PRIVATE
    pico_stdlib
    hardware_pio
    hardware_clocks
)

pico_enable_stdio_usb(dac_wave_test 0)
pico_enable_stdio_uart(dac_wave_test 0)
pico_add_extra_outputs(dac_wave_test)
