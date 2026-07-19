cmake_minimum_required(VERSION 3.14)

# We need to compile for the RP2350
set(PICO_PLATFORM rp2350 CACHE STRING "Pico Platform")
set(PICO_BOARD "pico2" CACHE STRING "Board type")

# Include pico_sdk_import before project()
include(pico_sdk_import.cmake)

project(LibreCom C CXX ASM)

# Initialize the Pico SDK
pico_sdk_init()

# Provide stub syscalls for codec2 auxiliary executables built on bare metal
add_link_options("--specs=nosys.specs")

# Fetch Codec 2
include(FetchContent)
set(UNITTEST OFF CACHE BOOL "Disable codec2 unit tests" FORCE)
set(BUILD_FREEDV_API OFF CACHE BOOL "Disable FreeDV API" FORCE)
set(INSTALL OFF CACHE BOOL "Disable install" FORCE)

message(STATUS "Fetching Codec 2...")
FetchContent_Declare(
    codec2
    GIT_REPOSITORY https://github.com/drowe67/codec2.git
    GIT_TAG        main
)
FetchContent_GetProperties(codec2)
if(NOT codec2_POPULATED)
  FetchContent_Populate(codec2)
  # Patch the CMakeLists.txt to fix the .exe issue on Windows cross-compile
  file(READ ${codec2_SOURCE_DIR}/src/CMakeLists.txt CODEC2_CMAKE)
  if(NOT CODEC2_CMAKE MATCHES "generate_codebook.exe")
      string(REPLACE "codec2_native/src/generate_codebook \${CMAKE_CURRENT_BINARY_DIR}" "codec2_native/src/generate_codebook.exe \${CMAKE_CURRENT_BINARY_DIR}" CODEC2_CMAKE "${CODEC2_CMAKE}")
      string(REPLACE "\${CMAKE_CURRENT_BINARY_DIR}/generate_codebook)" "\${CMAKE_CURRENT_BINARY_DIR}/generate_codebook.exe)" CODEC2_CMAKE "${CODEC2_CMAKE}")
      string(REPLACE "BUILD_BYPRODUCTS \${CMAKE_CURRENT_BINARY_DIR}/generate_codebook" "BUILD_BYPRODUCTS \${CMAKE_CURRENT_BINARY_DIR}/generate_codebook.exe" CODEC2_CMAKE "${CODEC2_CMAKE}")
      file(WRITE ${codec2_SOURCE_DIR}/src/CMakeLists.txt "${CODEC2_CMAKE}")
  endif()
  
  # Patch modem_probe.c to fix 32-bit ARM compiler pointer incompatibility
  file(READ ${codec2_SOURCE_DIR}/src/modem_probe.c MODEM_PROBE_C)
  string(REPLACE "(int32_t *)dbuf" "(int *)dbuf" MODEM_PROBE_C "${MODEM_PROBE_C}")
  file(WRITE ${codec2_SOURCE_DIR}/src/modem_probe.c "${MODEM_PROBE_C}")
  
  # Prevent codec2 from building any auxiliary executables and broken exports
  file(READ ${codec2_SOURCE_DIR}/src/CMakeLists.txt CODEC2_CMAKE)
  if(NOT CODEC2_CMAKE MATCHES "return\\(\\)\ninstall\\(EXPORT codec2-config")
      string(REPLACE "install(EXPORT codec2-config" "return()\ninstall(EXPORT codec2-config" CODEC2_CMAKE "${CODEC2_CMAKE}")
      file(WRITE ${codec2_SOURCE_DIR}/src/CMakeLists.txt "${CODEC2_CMAKE}")
  endif()
  
  # Prevent codec2 from building demo tools
  file(READ ${codec2_SOURCE_DIR}/CMakeLists.txt CODEC2_ROOT_CMAKE)
  if(NOT CODEC2_ROOT_CMAKE MATCHES "#add_subdirectory\\(demo\\)")
      string(REPLACE "add_subdirectory(demo)" "#add_subdirectory(demo)" CODEC2_ROOT_CMAKE "${CODEC2_ROOT_CMAKE}")
      file(WRITE ${codec2_SOURCE_DIR}/CMakeLists.txt "${CODEC2_ROOT_CMAKE}")
  endif()
endif()

add_subdirectory(${codec2_SOURCE_DIR} ${codec2_BINARY_DIR})



# Add shared library for common functions
add_library(shared_lib
    shared/audio_clocks.c
)
target_include_directories(shared_lib PUBLIC shared)
target_link_libraries(shared_lib PUBLIC pico_stdlib hardware_pwm hardware_clocks)

# -----------------
# TX Firmware Target
# -----------------
add_executable(tx
    tx/tx_main.c
)

pico_generate_pio_header(tx ${CMAKE_CURRENT_LIST_DIR}/shared/i2s_pio.pio)

target_include_directories(tx PRIVATE 
    shared
    ${codec2_SOURCE_DIR}/src
    ${codec2_BINARY_DIR}
)

target_link_libraries(tx PRIVATE
    pico_stdlib
    hardware_uart
    hardware_pio
    pico_multicore
    codec2
    shared_lib
    m
)

# Enable USB CDC (serial) for debug output, disable standard UART output so it doesn't interfere
pico_enable_stdio_usb(tx 1)
pico_enable_stdio_uart(tx 0)

# -----------------
# DAC Test Target
# -----------------
add_executable(dac_test
    dac_test/dac_test_main.c
)

pico_generate_pio_header(dac_test ${CMAKE_CURRENT_LIST_DIR}/shared/i2s_pio.pio)

target_include_directories(dac_test PRIVATE shared)

target_link_libraries(dac_test PRIVATE
    pico_stdlib
    hardware_pio
    shared_lib
    m
)

pico_enable_stdio_usb(dac_test 1)
pico_enable_stdio_uart(dac_test 0)

# Create uf2 etc. for all targets
pico_add_extra_outputs(tx)
pico_add_extra_outputs(dac_test)


# -----------------
# RX Firmware Target
# -----------------
add_executable(rx
    rx/rx_main.c
)

pico_generate_pio_header(rx ${CMAKE_CURRENT_LIST_DIR}/shared/i2s_pio.pio)

target_include_directories(rx PRIVATE 
    shared
    ${codec2_SOURCE_DIR}/src
    ${codec2_BINARY_DIR}
)

target_link_libraries(rx PRIVATE
    pico_stdlib
    hardware_uart
    hardware_pio
    pico_multicore
    codec2
    shared_lib
    m
)

# Enable USB CDC (serial) for debug output, disable standard UART output so it doesn't interfere
pico_enable_stdio_usb(rx 1)
pico_enable_stdio_uart(rx 0)

pico_add_extra_outputs(rx)

# -----------------
# Raw Audio TX Firmware Target
# -----------------
add_executable(raw_tx
    raw_tx/raw_tx_main.c
)

pico_generate_pio_header(raw_tx ${CMAKE_CURRENT_LIST_DIR}/shared/i2s_pio.pio)
target_include_directories(raw_tx PRIVATE shared)
target_link_libraries(raw_tx PRIVATE pico_stdlib hardware_uart hardware_pio pico_multicore shared_lib)
pico_enable_stdio_usb(raw_tx 1)
pico_enable_stdio_uart(raw_tx 0)
pico_add_extra_outputs(raw_tx)

# -----------------
# Raw Audio RX Firmware Target
# -----------------
add_executable(raw_rx
    raw_rx/raw_rx_main.c
)

pico_generate_pio_header(raw_rx ${CMAKE_CURRENT_LIST_DIR}/shared/i2s_pio.pio)
target_include_directories(raw_rx PRIVATE shared)
target_link_libraries(raw_rx PRIVATE pico_stdlib hardware_uart hardware_pio pico_multicore shared_lib)
pico_enable_stdio_usb(raw_rx 1)
pico_enable_stdio_uart(raw_rx 0)
pico_add_extra_outputs(raw_rx)
