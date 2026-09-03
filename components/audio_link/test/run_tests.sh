#!/bin/bash
# Host-side tests for the audio_link component. These need no hardware and no
# ESP-IDF - they compile the same audio_link.c the firmware uses with the
# system gcc, so a framing or companding regression shows up in a second
# instead of on the bench.
#
#   ./run_tests.sh
set -u

cd "$(dirname "$0")"
SRC=../audio_link.c
INC=../include
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
fails=0

run() {
    local name=$1; shift
    printf '=== %s ===\n' "$name"
    if ! gcc -Wall -Wextra -O1 -I "$INC" -o "$OUT/$name.exe" "$@" "$SRC" -lm; then
        echo "BUILD FAILED"
        fails=$((fails + 1))
        return
    fi
    "$OUT/$name.exe" || fails=$((fails + 1))
    echo
}

# Framing: clean round trip, sync bytes appearing inside the payload,
# recovery after corruption, sequence wrap, gap detection, 320 B payloads.
run test_framing test_framing.c

# G.711 mu-law: round-trip SNR, full-range error, codebook coverage.
run test_ulaw test_ulaw.c

# Not a test - a model showing how frame size multiplies the effect of line
# errors. This is why the raw-PCM bring-up stage uses mu-law at 115200 rather
# than 16-bit at 460800. See BRINGUP.md, stage 1.
run ber_model ber_model.c

if [ "$fails" -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "$fails TEST BINARY/BINARIES FAILED"
fi
exit "$fails"
