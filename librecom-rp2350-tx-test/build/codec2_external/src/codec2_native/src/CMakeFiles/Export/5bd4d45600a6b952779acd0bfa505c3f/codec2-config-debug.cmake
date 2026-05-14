#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "codec2" for configuration "Debug"
set_property(TARGET codec2 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(codec2 PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/lib/libcodec2.dll.a"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/bin/libcodec2.dll"
  )

list(APPEND _cmake_import_check_targets codec2 )
list(APPEND _cmake_import_check_files_for_codec2 "${_IMPORT_PREFIX}/lib/libcodec2.dll.a" "${_IMPORT_PREFIX}/bin/libcodec2.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
