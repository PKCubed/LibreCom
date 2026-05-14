# Install script for directory: C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/CODEC2")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/msys64/ucrt64/bin/objdump.exe")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config.cmake"
         "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/src/CMakeFiles/Export/5bd4d45600a6b952779acd0bfa505c3f/codec2-config.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2" TYPE FILE FILES "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/src/CMakeFiles/Export/5bd4d45600a6b952779acd0bfa505c3f/codec2-config.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2" TYPE FILE FILES "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/src/CMakeFiles/Export/5bd4d45600a6b952779acd0bfa505c3f/codec2-config-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/src/libcodec2.dll.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/src/libcodec2.dll")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcodec2.dll" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcodec2.dll")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "C:/msys64/ucrt64/bin/strip.exe" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/libcodec2.dll")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/codec2" TYPE FILE FILES
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2_fdmdv.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2_cohpsk.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2_fm.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2_ofdm.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/fsk.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2_fifo.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/comp.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/modem_stats.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/freedv_api.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/reliable_text.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/external/codec2/src/codec2_math.h"
    "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/codec2/version.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/peter/Documents/GitHub/LibreCom/librecom-rp2350-tx-test/build/codec2_external/src/codec2_native/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
