# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(librecom-rp2040-tx-test C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# 1. Force the compiler into maximum optimization (Release)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)

# 2. Force GCC 14 to ignore the legacy pointer mismatches
add_compile_options(-Wno-incompatible-pointer-types)

# 3. Aggressively tell Codec 2 to stop building desktop tools
set(UNITTEST OFF CACHE BOOL "Disable Codec2 tests" FORCE)
set(BUILD_EXECUTABLES OFF CACHE BOOL "Disable Codec2 executables" FORCE)
# ---------------------------------------------------------

# Tell the ARM compiler to use dummy system calls for any rogue executables
add_link_options(--specs=nosys.specs)

# ---------------------------------------------------------
# CODEC 2 INTEGRATION
# ---------------------------------------------------------
set(UNITTEST OFF CACHE BOOL "Disable Codec2 tests")
set(BUILD_EXECUTABLES OFF CACHE BOOL "Disable Codec2 executables")

# Point CMake to the generator you built in Phase 1
set(GENERATE_CODEBOOK "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2040-tx-test/codec2/build_native/src/generate_codebook.exe" CACHE FILEPATH "")

link_libraries(m) # Force the math library into everything

# Add the Codec 2 directory to your build
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codec2 codec2_build)
# ---------------------------------------------------------

# Add executable. Default name is the project name, version 0.1
add_executable(librecom-rp2040-tx-test main.c)

pico_generate_pio_header(librecom-rp2040-tx-test ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)
pico_generate_pio_header(librecom-rp2040-tx-test ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)

pico_set_program_name(librecom-rp2040-tx-test "librecom-rp2040-tx-test")
pico_set_program_version(librecom-rp2040-tx-test "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(librecom-rp2040-tx-test 0)
pico_enable_stdio_usb(librecom-rp2040-tx-test 1)

# Add the standard library and hardware components to the build
target_link_libraries(librecom-rp2040-tx-test
    pico_stdlib
    hardware_pio 
    hardware_pwm 
    hardware_uart
    pico_multicore   # Added for Core 1
    codec2           # Added for compression
    m
)

# Add the standard include files to the build
target_include_directories(librecom-rp2040-tx-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/codec2_build
)

pico_add_extra_outputs(librecom-rp2040-tx-test)

# =========================================================
# RECEIVER (RX) CONFIGURATION
# =========================================================
add_executable(librecom-rp2040-rx rx_main.c)

# Generate the TX PIO header
pico_generate_pio_header(librecom-rp2040-rx ${CMAKE_CURRENT_LIST_DIR}/i2s_tx.pio)

# Link the same libraries
target_link_libraries(librecom-rp2040-rx
    pico_stdlib
    hardware_pio 
    hardware_clocks
    hardware_uart
    pico_multicore   
    codec2           
    m                
)

pico_enable_stdio_uart(librecom-rp2040-rx 0)
pico_enable_stdio_usb(librecom-rp2040-rx 1)

target_include_directories(librecom-rp2040-rx PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/codec2_build
)

pico_add_extra_outputs(librecom-rp2040-rx)


# =========================================================
# I2S SINE WAVE TEST (STANDALONE)
# =========================================================
# 1. Define the new executable and its source file
add_executable(rx_i2s_test rx_I2S_test.c)

# 2. Tell it to generate the PIO header for this specific executable
pico_generate_pio_header(rx_i2s_test ${CMAKE_CURRENT_LIST_DIR}/i2s_tx.pio)

# 3. Link only the libraries needed for the test (No Codec2 needed here!)
target_link_libraries(rx_i2s_test
    pico_stdlib
    hardware_pio 
    hardware_clocks
    m                
)

# 4. Enable USB printing just in case
pico_enable_stdio_uart(rx_i2s_test 0)
pico_enable_stdio_usb(rx_i2s_test 1)

# 5. Generate the final .uf2 file
pico_add_extra_outputs(rx_i2s_test)