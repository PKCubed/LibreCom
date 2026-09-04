# Codec 2 as an ESP-IDF component. This file is local to this project and does
# not exist upstream - see VENDORED.md.

# Highest fundamental frequency Codec 2 will model, in Hz.
#
# Codec 2 is a harmonic vocoder: it estimates a pitch and resynthesises speech
# as harmonics of it. Upstream fixes the ceiling at 400 Hz (P_MIN_S 0.0025).
# Above that, encode_Wo() clamps the quantiser index, so a higher voice is
# coded at a wrong fundamental and the resynthesis garbles - which is what
# high voices sound like on the stock setting.
#
# We ran at 500 Hz briefly to chase a "high voices garble" report. It turned
# out the real symptom was the voice cracking up a register for a few tens of
# milliseconds, which is an octave error in the estimator, not a ceiling
# problem - and a higher ceiling arguably makes that worse, because 2*F0 for a
# 210-250 Hz speaker then falls inside the searchable range. Back to stock.
#
# Raising it to 500 Hz covers children and high adult voices. The cost is
# resolution: the pitch is quantised into 7 bits (128 levels) linearly across
# the whole range, so widening it from 50-400 to 50-500 Hz coarsens the step
# from 2.73 Hz to 3.52 Hz for everyone. That is a much smaller error than
# clamping, but it is not free.
#
# Both the encoder and the decoder derive their quantiser bounds from this, so
# EVERY BOARD ON A LINK MUST BE BUILT WITH THE SAME VALUE. Setting it here, in
# the shared component, is what guarantees that.
#
# Set to 400 to get bit-exact upstream behaviour.
# Deliberately NOT a CACHE variable. A cached value survives edits to this
# file, so the build silently keeps using whatever was configured first - which
# already caused one round of "the binary does not match the source" confusion
# here. A plain set() makes this file the single source of truth.
set(CODEC2_PITCH_MAX_HZ 400)

# Threshold at which post_process_sub_multiples() in nlp.c accepts a LOWER
# sub-multiple of the pitch peak. Upstream is 0.30 (halved to 0.15 when the
# candidate agrees with the previous frame, which is its pitch tracking).
#
# This is the knob for octave errors - the voice cracking up a register for a
# frame or two. Lower means the estimator is readier to take the lower, usually
# correct, octave. Too low and genuinely high voices get halved instead, which
# sounds gravelly. Watch the transmitter jump= counter while tuning.
#
# nlp() runs in the ENCODER only and does not change the bitstream format, so
# changing this needs the transmitter reflashed but not the receiver.
set(CODEC2_CNLP 0.30)

file(GLOB srcs "src/*.c")
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "src")

# PUBLIC so dependants can report the configured range in their boot banner.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
                           CODEC2_PITCH_MAX_HZ=${CODEC2_PITCH_MAX_HZ}
                           CNLP=${CODEC2_CNLP})

message(STATUS "Codec 2: pitch ceiling ${CODEC2_PITCH_MAX_HZ} Hz, CNLP ${CODEC2_CNLP}")
