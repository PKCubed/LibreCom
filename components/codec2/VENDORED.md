# Vendored: esp32_codec2_arduino

This directory is a copy of, not a submodule of:

- **Upstream:** https://github.com/sh123/esp32_codec2_arduino
- **Commit:** `15a27e1` ("Merge pull request #4 from sh123/codec2-dev")
- **Library version:** 1.0.7 (see `library.properties`)
- **Licence:** LGPL 2.1, see `LICENSE`

Upstream is itself a packaging of David Rowe's Codec 2 from
https://github.com/drowe67/codec2-dev.

## Why vendored rather than a submodule

`CMakeLists.txt` in this directory is **local to this project** — it is the
ESP-IDF component manifest and does not exist upstream. As a submodule it could
not be tracked by the parent repository, so a fresh clone would check out the
codec2 sources without the one file that makes them buildable.

Vendoring keeps `git clone && idf.py build` working with no extra steps.

## Local modifications

Two added files and one small patch:

- `CMakeLists.txt` *(added)* — registers `src/*.c` as an IDF component
  exporting `src/`, and sets `CODEC2_PITCH_MAX_HZ`.
- `test/` *(added)* — a host-only demonstration of what that setting changes.
- `src/defines.h` *(patched)* — `P_MIN_S` is now derived from
  `CODEC2_PITCH_MAX_HZ` when that is defined, and otherwise keeps its upstream
  value. The patch is three preprocessor lines and changes nothing unless the
  macro is set.

### Why P_MIN_S is configurable

`P_MIN_S` sets the shortest pitch period Codec 2 will model, so `1/P_MIN_S` is
the highest fundamental it can represent. Upstream fixes it at 400 Hz. Above
that, `encode_Wo()` clamps the quantiser index, so a 450 Hz voice is coded as
397 Hz — not merely detuned, but resynthesised on the wrong fundamental, which
garbles every harmonic. Children and high adult voices land right there.

We build at the stock 400 Hz. 500 Hz was tried and reverted: it did not fix the
artifact we were chasing, it coarsens the pitch step from 2.73 Hz to 3.52 Hz for
every speaker, and it breaks compatibility with standard Codec 2. Run
`test/run_pitch_test.sh` to see both behaviours side by side.

`src/nlp.c` is patched the same way to make `CNLP` overridable - the threshold
controlling how readily the estimator accepts a lower sub-multiple, which is
what governs octave errors.

**Both ends of a link must use the same value.** Encoder and decoder derive
their quantiser bounds from it, so a mismatch decodes speech at the wrong
pitch. Setting it once in `CMakeLists.txt`, which both projects share, is what
prevents that. Both firmwares also print the configured range at boot.

Updating to a newer upstream means copying its tree over the top, then
restoring `CMakeLists.txt`, `test/` and the three-line `defines.h` guard.

## Notes for anyone updating this

- `codec2_math_arm.c` compiles to nothing on Xtensa; it is guarded on
  `__EMBEDDED__ && __ARM_ARCH`. The generic C paths are used instead.
- The `freedv_*`, `ofdm*` and FEC sources are compiled but unused by this
  project, which only calls `codec2_create` / `codec2_encode` / `codec2_decode`.
  `cohpsk.c` is absent upstream, so those objects will not link on a host build
  — see `components/audio_link/test/run_tests.sh` for the codec-only file list
  used when building Codec 2 off-target.
