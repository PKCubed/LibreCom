#!/bin/bash
# Shows what CODEC2_PITCH_MAX_HZ actually does, by running F0 through the real
# encode_Wo/decode_Wo quantiser both ways. Host only, no ESP-IDF needed.
#
# Upstream clamps everything above 400 Hz onto one quantiser index, so a high
# voice is resynthesised at the wrong fundamental. See ../CMakeLists.txt.
set -u
cd "$(dirname "$0")"
SRC=../src
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

CORE="codec2.c codebook.c codebookd.c codebookge.c codebookjmv.c codebookjvm.c
      codebooklspmelvq.c codebookmel.c codebooknewamp1.c codebooknewamp1_energy.c
      codebooknewamp2.c codebooknewamp2_energy.c codec2_fft.c dump.c interp.c
      lpc.c lsp.c mbest.c newamp1.c newamp2.c nlp.c pack.c phase.c postfilter.c
      quantise.c sine.c kiss_fft.c kiss_fftr.c"
FILES=""; for f in $CORE; do FILES="$FILES $SRC/$f"; done

for hz in "" 500; do
    if [ -z "$hz" ]; then D=""; label="UPSTREAM (400 Hz ceiling)";
    else D="-DCODEC2_PITCH_MAX_HZ=$hz"; label="OVERRIDE ($hz Hz ceiling)"; fi
    echo "================= $label ================="
    gcc -O1 -I "$SRC" $D -o "$OUT/pitch.exe" pitch_range.c $FILES -lm || exit 1
    "$OUT/pitch.exe"
    echo
done
