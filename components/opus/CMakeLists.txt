# libopus as an ESP-IDF component. This file is local to this project and does
# not exist upstream - see VENDORED.md.
#
# Built in FIXED_POINT mode. The ESP32-S3 has a single-precision FPU, but Opus
# fixed-point is the well-trodden embedded path: it is what upstream tests on
# small targets, and it avoids float in the hot SILK loops.

file(GLOB opus_celt   "celt/*.c")
file(GLOB opus_silk   "silk/*.c")
file(GLOB opus_fixed  "silk/fixed/*.c")
file(GLOB opus_src    "src/*.c")

idf_component_register(SRCS ${opus_celt} ${opus_silk} ${opus_fixed} ${opus_src}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "celt" "silk" "silk/fixed" "src")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD                  # required by upstream for an in-tree build
    FIXED_POINT                 # integer SILK/CELT, selects silk/fixed
    DISABLE_FLOAT_API           # we only ever call the int16 entry points
    VAR_ARRAYS                  # scratch via C99 VLAs rather than a global
    PACKAGE_VERSION="1.4-librecom"   # what opus.c actually reads for its version banner
)

# Opus is the only heavy DSP in this project and it has to hold a 20 ms frame
# deadline. It is worth -O3 even though the rest of the build is -O2.
#
# -w disables warnings for this component only. IDF compiles with -Werror=all,
# and GCC's -Wno-error deliberately does not cancel a specific -Werror=<kind>,
# so the alternative is chasing upstream's false positives one at a time
# (stringop-overflow in decode_indices.c, stringop-overread in main.h, ...).
# This is released upstream code we do not modify; our own components keep the
# full strict warning set.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -w)
