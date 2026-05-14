# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/dev/pico-sdk/tools/pioasm")
  file(MAKE_DIRECTORY "C:/dev/pico-sdk/tools/pioasm")
endif()
file(MAKE_DIRECTORY
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pioasm"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pioasm-install"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/tmp"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
