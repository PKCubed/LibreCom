# opencore-amrnb as an ESP-IDF component. This file is local to this project
# and does not exist upstream - see VENDORED.md.
#
# AMR-NB is fixed point throughout (it was written for DSPs), so there is no
# float/integer build choice to make. The sources are .cpp but are C in style;
# IDF compiles them as C++ with exceptions and RTTI off, which is fine.
#
# Globbing is safe here only because the seven sources upstream deliberately
# excludes (bits2prm, copy, div_32, l_abs, r_fft, vad1, vad2 - commented out in
# its Makefile.am, and their headers are not even shipped) have been deleted
# from the vendored tree. See VENDORED.md. If a future update reintroduces
# unbuildable files, this glob will pick them up and fail loudly rather than
# silently, which is the behaviour we want.

file(GLOB amr_common "opencore/codecs_v2/audio/gsm_amr/amr_nb/common/src/*.cpp")
file(GLOB amr_dec    "opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src/*.cpp")
file(GLOB amr_enc    "opencore/codecs_v2/audio/gsm_amr/amr_nb/enc/src/*.cpp")

idf_component_register(
    SRCS "amrnb/wrapper.cpp" ${amr_common} ${amr_dec} ${amr_enc}
    INCLUDE_DIRS "amrnb"
    PRIV_INCLUDE_DIRS
        "oscl"
        "opencore/codecs_v2/audio/gsm_amr/amr_nb/common/include"
        "opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/include"
        "opencore/codecs_v2/audio/gsm_amr/amr_nb/dec/src"
        "opencore/codecs_v2/audio/gsm_amr/amr_nb/enc/include"
        "opencore/codecs_v2/audio/gsm_amr/amr_nb/enc/src"
        "opencore/codecs_v2/audio/gsm_amr/common/dec/include")

# -w for the same reason as the opus component: this is upstream code we do not
# modify, IDF builds with -Werror=all, and GCC's -Wno-error does not cancel a
# specific -Werror=<kind>. Our own components keep the strict warning set.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -w)
