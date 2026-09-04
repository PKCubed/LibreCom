# Vendored: libopus

- **Upstream:** https://downloads.xiph.org/releases/opus/opus-1.4.tar.gz
- **Version:** 1.4
- **Licence:** BSD-3-Clause, see `COPYING`

## Why 1.4 rather than 1.5.x

1.5 added the ML-based DRED and LACE/NoLACE extensions, which bring in neural
network weights and want considerably more flash and RAM. Nothing here needs
them, and 1.4 is the leaner target for a microcontroller.

## Why vendored rather than a registry component

The only Opus in the ESP component registry is inside
`espressif/esp_audio_codec`, which is a precompiled binary under the "Espressif
Modified MIT" licence: usable **only in conjunction with Espressif products**,
with redistribution for non-Espressif hardware prohibited. That is a poor fit
for a project called LibreCom, and it would become a real problem if the design
ever moved to another chip. Upstream libopus is BSD-3-Clause with no such
condition.

## What was removed

Only files we never compile, to keep the tree reviewable:

- `celt/arm`, `celt/mips`, `celt/x86`, and the matching `silk/` directories -
  architecture-specific assembly and intrinsics. The Xtensa build uses the
  generic C paths.
- `silk/float` - superseded by `silk/fixed` in a `FIXED_POINT` build.
- `celt/tests`, `silk/tests`.
- `src/opus_demo.c`, `src/repacketizer_demo.c`, `src/opus_compare.c` - command
  line tools with their own `main()`.
- autotools and the upstream CMake build.

That leaves 134 `.c` files. No upstream source file is modified.

## Build configuration

`CMakeLists.txt` here is local to this project. It sets:

| Define | Why |
|---|---|
| `OPUS_BUILD` | required by upstream for an in-tree build |
| `FIXED_POINT` | integer SILK and CELT; selects `silk/fixed` |
| `DISABLE_FLOAT_API` | we only call the `int16` entry points, and this drops the float-only analysis code |
| `VAR_ARRAYS` | scratch allocation via C99 VLAs rather than a shared global buffer, which keeps it usable from more than one task |

Compiled `-O3` (the rest of the project is `-O2`) because Opus is the only
heavy DSP here and has to hold a 20 ms frame deadline.

Compiled `-w`. IDF builds with `-Werror=all`, and GCC's `-Wno-error`
deliberately does **not** cancel a specific `-Werror=<kind>`, so upstream's
known false positives (`stringop-overflow` in `silk/decode_indices.c`,
`stringop-overread` via `silk/main.h`) would otherwise each need chasing
individually. This is released code we do not modify; our own components keep
the full strict warning set.

## Size

Opus costs roughly 200 KB of flash. With Codec 2 also linked the transmitter
reached 70% of the 1 MB app partition, so if both codecs are ever wanted at
once, raise `CONFIG_ESPTOOLPY_FLASHSIZE` from 2 MB and use a larger partition
table.

## Updating

Copy a newer upstream tree over the top, delete the directories listed above,
and keep `CMakeLists.txt` and this file. Nothing else is patched.
