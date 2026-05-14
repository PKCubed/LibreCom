cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Point it to your pico-sdk directory.")
endif()

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2350_codec2_audio_link C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Optional override for out-of-tree Codec2 installs.
set(CODEC2_ROOT "" CACHE PATH "Root folder of Codec2 install")

set(VENDORED_CODEC2_DIR ${CMAKE_CURRENT_LIST_DIR}/external/codec2)
set(VENDORED_CODEC2_BINARY_DIR ${CMAKE_BINARY_DIR}/codec2_external)

find_path(CODEC2_INCLUDE_DIR
    NAMES codec2/codec2.h codec2.h
    HINTS ${CODEC2_ROOT}
    PATH_SUFFIXES include
)

find_library(CODEC2_LIBRARY
    NAMES codec2
    HINTS ${CODEC2_ROOT}
    PATH_SUFFIXES lib
)

if(NOT CODEC2_INCLUDE_DIR OR NOT CODEC2_LIBRARY)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CODEC2_PC QUIET IMPORTED_TARGET codec2)
    endif()
endif()

if(NOT CODEC2_LIBRARY AND NOT TARGET PkgConfig::CODEC2_PC)
    if(EXISTS ${VENDORED_CODEC2_DIR}/CMakeLists.txt)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(UNITTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory(${VENDORED_CODEC2_DIR} ${VENDORED_CODEC2_BINARY_DIR} EXCLUDE_FROM_ALL)

        if(TARGET codec2)
            set(CODEC2_TARGET codec2)
            set(CODEC2_INCLUDE_DIR ${VENDORED_CODEC2_DIR}/src)
            set(CODEC2_GENERATED_INCLUDE_DIR ${VENDORED_CODEC2_BINARY_DIR})
        endif()
    endif()
endif()

if(NOT CODEC2_LIBRARY AND NOT TARGET PkgConfig::CODEC2_PC AND NOT TARGET codec2)
    message(FATAL_ERROR
        "Codec2 not found. Install libcodec2-dev, set -DCODEC2_ROOT=<codec2-install-prefix>, or clone codec2 to external/codec2.")
endif()

set(AUDIO_COMMON_LIBS
    pico_stdlib
    hardware_dma
    hardware_irq
    hardware_pio
    hardware_uart
    hardware_clocks
)

set(AUDIO_COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}
)

if(CODEC2_INCLUDE_DIR)
    list(APPEND AUDIO_COMMON_INCLUDES ${CODEC2_INCLUDE_DIR})
endif()

if(CODEC2_GENERATED_INCLUDE_DIR)
    list(APPEND AUDIO_COMMON_INCLUDES ${CODEC2_GENERATED_INCLUDE_DIR})
endif()

function(add_audio_target target_name source_file)
    add_executable(${target_name} ${source_file})

    pico_generate_pio_header(${target_name}
        ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio
    )

    target_include_directories(${target_name} PRIVATE ${AUDIO_COMMON_INCLUDES})
    target_link_libraries(${target_name} PRIVATE ${AUDIO_COMMON_LIBS})

    if(TARGET codec2)
        target_link_libraries(${target_name} PRIVATE codec2)
    elseif(CODEC2_LIBRARY)
        target_link_libraries(${target_name} PRIVATE ${CODEC2_LIBRARY})
    elseif(TARGET PkgConfig::CODEC2_PC)
        target_link_libraries(${target_name} PRIVATE PkgConfig::CODEC2_PC)
    endif()

    pico_enable_stdio_usb(${target_name} 0)
    pico_enable_stdio_uart(${target_name} 0)
    pico_add_extra_outputs(${target_name})
endfunction()

add_audio_target(audio_link_tx main_tx.c)
add_audio_target(audio_link_rx main_rx.c)
