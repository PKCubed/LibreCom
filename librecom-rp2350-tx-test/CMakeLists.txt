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
set(CODEC2_ROOT "$ENV{CODEC2_ROOT}" CACHE PATH "Root folder of Codec2 install")
if((NOT CODEC2_ROOT) AND DEFINED ENV{CODEC2_ROOT} AND NOT "$ENV{CODEC2_ROOT}" STREQUAL "")
    set(CODEC2_ROOT "$ENV{CODEC2_ROOT}" CACHE PATH "Root folder of Codec2 install" FORCE)
endif()

set(CODEC2_HINT_DIRS)
if(CODEC2_ROOT)
    list(APPEND CODEC2_HINT_DIRS ${CODEC2_ROOT})
    get_filename_component(_codec2_root_parent "${CODEC2_ROOT}" DIRECTORY)
    if(_codec2_root_parent)
        list(APPEND CODEC2_HINT_DIRS ${_codec2_root_parent})
    endif()
endif()

if(DEFINED ENV{CODEC2_ROOT} AND NOT "$ENV{CODEC2_ROOT}" STREQUAL "")
    list(APPEND CODEC2_HINT_DIRS $ENV{CODEC2_ROOT})
    get_filename_component(_codec2_env_parent "$ENV{CODEC2_ROOT}" DIRECTORY)
    if(_codec2_env_parent)
        list(APPEND CODEC2_HINT_DIRS ${_codec2_env_parent})
    endif()
endif()

list(REMOVE_DUPLICATES CODEC2_HINT_DIRS)

set(VENDORED_CODEC2_DIR ${CMAKE_CURRENT_LIST_DIR}/external/codec2)
set(VENDORED_CODEC2_BINARY_DIR ${CMAKE_BINARY_DIR}/codec2_external)
set(CODEC2_SOURCE_DIR "")

if(EXISTS ${VENDORED_CODEC2_DIR}/CMakeLists.txt)
    set(CODEC2_SOURCE_DIR ${VENDORED_CODEC2_DIR})
endif()

find_path(CODEC2_INCLUDE_DIR
    NAMES codec2/codec2.h codec2.h
    HINTS ${CODEC2_HINT_DIRS}
    PATH_SUFFIXES include install/include src
)

find_library(CODEC2_LIBRARY
    NAMES libcodec2 codec2
    HINTS ${CODEC2_HINT_DIRS}
    PATH_SUFFIXES lib install/lib src
)

if(NOT CODEC2_INCLUDE_DIR)
    foreach(_base IN LISTS CODEC2_HINT_DIRS)
        if(EXISTS "${_base}/install/include/codec2/codec2.h")
            set(CODEC2_INCLUDE_DIR "${_base}/install/include")
            break()
        endif()
    endforeach()
endif()

if(NOT CODEC2_LIBRARY)
    foreach(_base IN LISTS CODEC2_HINT_DIRS)
        if(EXISTS "${_base}/install/lib/libcodec2.a")
            set(CODEC2_LIBRARY "${_base}/install/lib/libcodec2.a")
            break()
        elseif(EXISTS "${_base}/install/lib/codec2.lib")
            set(CODEC2_LIBRARY "${_base}/install/lib/codec2.lib")
            break()
        endif()
    endforeach()
endif()

if(CODEC2_ROOT)
    get_filename_component(_codec2_root_parent2 "${CODEC2_ROOT}" DIRECTORY)
    if(EXISTS "${_codec2_root_parent2}/install/include/codec2/codec2.h")
        set(CODEC2_INCLUDE_DIR "${_codec2_root_parent2}/install/include")
    endif()
    if(EXISTS "${_codec2_root_parent2}/install/lib/libcodec2.a")
        set(CODEC2_LIBRARY "${_codec2_root_parent2}/install/lib/libcodec2.a")
    elseif(EXISTS "${_codec2_root_parent2}/install/lib/codec2.lib")
        set(CODEC2_LIBRARY "${_codec2_root_parent2}/install/lib/codec2.lib")
    endif()
endif()

if((NOT CODEC2_INCLUDE_DIR OR NOT CODEC2_LIBRARY) AND NOT CMAKE_CROSSCOMPILING)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CODEC2_PC QUIET IMPORTED_TARGET codec2)
    endif()
endif()

if(CODEC2_SOURCE_DIR)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(UNITTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CODEC2_SOURCE_DIR} ${VENDORED_CODEC2_BINARY_DIR} EXCLUDE_FROM_ALL)

    if(TARGET codec2)
        set(CODEC2_TARGET codec2)
        set(CODEC2_INCLUDE_DIR ${CODEC2_SOURCE_DIR}/src)
        set(CODEC2_GENERATED_INCLUDE_DIR ${VENDORED_CODEC2_BINARY_DIR})
        # Prefer cross-compiled codec2 target over any host prebuilt library.
        set(CODEC2_LIBRARY "")
    endif()
endif()

if(CODEC2_LIBRARY OR TARGET PkgConfig::CODEC2_PC OR TARGET codec2)
    set(CODEC2_AVAILABLE ON)
else()
    set(CODEC2_AVAILABLE OFF)
    message(WARNING
        "Codec2 not found. audio_link_tx/audio_link_rx targets will be disabled. "
        "Install libcodec2-dev, set -DCODEC2_ROOT=<codec2-install-prefix>, or clone codec2 to external/codec2 to enable them.")
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

function(add_passthrough_target target_name source_file)
    add_executable(${target_name} ${source_file})

    pico_generate_pio_header(${target_name}
        ${CMAKE_CURRENT_LIST_DIR}/i2s_audio.pio
    )

    target_include_directories(${target_name} PRIVATE ${AUDIO_COMMON_INCLUDES})
    target_link_libraries(${target_name} PRIVATE ${AUDIO_COMMON_LIBS})

    pico_enable_stdio_usb(${target_name} 0)
    pico_enable_stdio_uart(${target_name} 0)
    pico_add_extra_outputs(${target_name})
endfunction()

if(CODEC2_AVAILABLE)
    add_audio_target(audio_link_tx main_tx.c)
    add_audio_target(audio_link_rx main_rx.c)
endif()
add_audio_target(audio_sine_tx main_sine_tx.c)
add_passthrough_target(audio_passthrough_tx main_passthrough_tx.c)
add_passthrough_target(audio_passthrough_rx main_passthrough_rx.c)
