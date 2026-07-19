# As this script is run in a new cmake instance, it does not have access to
# the existing cache variables. Pass them in via the configure_file command.
set(CMAKE_BINARY_DIR C:/Users/peter/Documents/GitHub/LibreCom/build/_deps/codec2-build/src/codec2_native)
set(CMAKE_SOURCE_DIR C:/Users/peter/Documents/GitHub/LibreCom/build/_deps/codec2-src)
set(UNIX )
set(WIN32 1)
set(CMAKE_CROSSCOMPILING FALSE)
set(CMAKE_FIND_LIBRARY_SUFFIXES .dll.a;.a;.lib)
set(CMAKE_FIND_LIBRARY_PREFIXES lib;)
set(CMAKE_SYSTEM_LIBRARY_PATH C:/Program Files (x86)/CODEC2/bin;C:/Program Files/CMake/bin;/bin)
set(CMAKE_FIND_ROOT_PATH )
set(CODEC2_DLL ${CMAKE_BINARY_DIR}/src/libcodec2.dll)

include(${CMAKE_SOURCE_DIR}/cmake/GetPrerequisites.cmake)
get_prerequisites(${CODEC2_DLL} _deps 1 0 "" "")
foreach(_runtime ${_deps})
    message("Looking for ${_runtime}")
    find_library(RUNTIME_${_runtime} ${_runtime})
    message("${RUNTIME_${_runtime}}")
    if(RUNTIME_${_runtime})
        file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin"
        TYPE EXECUTABLE FILES "${RUNTIME_${_runtime}}")
    endif()
endforeach()
