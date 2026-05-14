# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/..")
  file(MAKE_DIRECTORY "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/..")
endif()
file(MAKE_DIRECTORY
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix/tmp"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix/src/codec2_native-stamp"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix/src"
  "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix/src/codec2_native-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix/src/codec2_native-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native-prefix/src/codec2_native-stamp${cfgdir}") # cfgdir has leading slash
endif()
