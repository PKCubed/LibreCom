# This is a helper included from the top level CMakeLists.txt

cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_FETCH_FROM_GIT ON CACHE BOOL "Fetch from git if PICO_SDK_PATH not set" FORCE)
endif()

if (PICO_SDK_FETCH_FROM_GIT AND NOT DEFINED PICO_SDK_PATH)
    include(FetchContent)
    message(STATUS "Fetching Pico SDK from git")
    FetchContent_Declare(
            pico_sdk
            GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk
            GIT_TAG        master
    )
    FetchContent_MakeAvailable(pico_sdk)
    set(PICO_SDK_PATH ${pico_sdk_SOURCE_DIR})
endif()

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
