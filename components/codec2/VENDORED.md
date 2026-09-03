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

Exactly one file is added; nothing upstream is patched:

- `CMakeLists.txt` — registers `src/*.c` as an IDF component exporting `src/`.

Keeping it to a single additive file means updating to a newer upstream is a
matter of copying its tree over the top and restoring this file.

## Notes for anyone updating this

- `codec2_math_arm.c` compiles to nothing on Xtensa; it is guarded on
  `__EMBEDDED__ && __ARM_ARCH`. The generic C paths are used instead.
- The `freedv_*`, `ofdm*` and FEC sources are compiled but unused by this
  project, which only calls `codec2_create` / `codec2_encode` / `codec2_decode`.
  `cohpsk.c` is absent upstream, so those objects will not link on a host build
  — see `components/audio_link/test/run_tests.sh` for the codec-only file list
  used when building Codec 2 off-target.
