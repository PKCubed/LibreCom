# Vendored: opencore-amrnb

- **Upstream:** https://sourceforge.net/projects/opencore-amr/ (opencore-amr 0.1.6)
- **Licence:** Apache-2.0, see `LICENSE`

This is the AMR-NB encoder and decoder from PacketVideo's OpenCORE, as shipped
with Android and packaged standalone by the opencore-amr project. It is the
usual open implementation of 3GPP TS 26.073.

## What AMR-NB is

Fixed **8 kHz sampling, 160 samples, 20 ms frames** — there is no sample rate or
frame size to choose, only one of eight bitrate modes from 4.75 to 12.2 kbps.
That means a 4 kHz audio bandwidth, the same ceiling Codec 2 had. At 12.2 kbps
it should sound clearly better than Codec 2 at 3.2 kbps, because it is CELP
waveform coding rather than a parametric vocoder, but it will not sound like
wideband Opus.

Everything is fixed point; it was written for DSPs, so there is no float build
to choose and no complexity knob.

## Layout

Upstream paths are preserved exactly, so updating is a matter of copying a newer
tree over the top:

```
amrnb/                 wrapper.cpp and the two public headers
oscl/                  3-header shim standing in for the Android OSCL layer
opencore/codecs_v2/audio/gsm_amr/
    amr_nb/common/     56 sources shared by encoder and decoder
    amr_nb/dec/        35 decoder sources
    amr_nb/enc/        61 encoder sources
```

The public API is small:

```c
void *Encoder_Interface_init(int dtx);
int   Encoder_Interface_Encode(void *st, enum Mode m, const short *pcm,
                               unsigned char *out, int forceSpeech);
void *Decoder_Interface_init(void);
void  Decoder_Interface_Decode(void *st, const unsigned char *in,
                               short *out, int bfi);
```

`Encoder_Interface_Encode` emits the storage/MIME format — one header byte
carrying the mode and quality bit, then the payload — and the decoder expects
the same, so a frame can be passed across the link untouched.

## What was removed, and why it matters

Ten sources present in the tarball are **not built by upstream**: they are
commented out in its `Makefile.am`, and for several of them the matching header
is not even shipped.

```
common/src:  bits2prm  copy  div_32  l_abs  r_fft  vad1  vad2
dec/src:     decoder_gsm_amr  pvgsmamrdecoder
enc/src:     gsmamr_encoder_wrapper
```

They are deleted here, because a `file(GLOB)` in the component would otherwise
pick them up and the build would fail on missing headers — which is exactly what
happened on the first attempt. The vendored tree was then diffed against the
source list parsed out of upstream's `Makefile.am` until the two agreed
(56 / 35 / 61). If you update this component, re-run that comparison rather than
trusting a glob.

The autotools files, the AMR-WB directory and the test programs are removed too.

No upstream source file is modified.

## Build configuration

`CMakeLists.txt` here is local to this project. The sources are `.cpp` but are C
in style; IDF compiles them as C++ with exceptions and RTTI off, which is fine.

Compiled `-O3 -w`, for the same reasons as the opus component: it is a frame
deadline worth optimising for, and IDF's `-Werror=all` cannot be undone with
`-Wno-error` on this kind of upstream code.

## Patents

The core AMR-NB patents date from the late 1990s and the usual 20-year terms
have run. That is not legal advice, but it is worth knowing that the situation
is different from when Codec 2 was written partly to sidestep them.
