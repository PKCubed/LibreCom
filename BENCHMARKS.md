# Measured codec numbers

Everything here was measured on the hardware — ESP32-S3 at 240 MHz, `-O2`
project-wide with the codec components at `-O3`, 20 ms frames. Nothing is
extrapolated. Reproduce with `TX_BENCH` in `transmitter/main/main.c`:

| `TX_BENCH` | what it runs |
|---|---|
| 1 | Opus sweep: sample rate × complexity × frame size |
| 2 | Opus multi-stream load |
| 3 | AMR-NB: all eight modes, then multi-stream load |
| 4 | all three codecs on identical axes, including delay |

## The table

Percentages are of the 20000 µs frame budget. `state` is encoder **and**
decoder together. `stack` is the measured high-water mark of a task doing both.

| codec | fs | encode | decode | B/frame | wire | state | stack | delay |
|---|---|---|---|---|---|---|---|---|
| Codec 2 3200 | 8 kHz | 5646 µs · **28%** | 5590 µs · **27%** | 8 | 5 kbps | 62.4 KB | 15.4 KB | 12 ms |
| Opus 12 kbps (NB) | 16 kHz | 6873 µs · 34% | 1514 µs · 7% | 30 | 14 kbps | 41.5 KB | 17.9 KB | 6 ms |
| Opus 16 kbps (WB) | 16 kHz | 9563 µs · 47% | 1729 µs · 8% | 40 | 18 kbps | 41.5 KB | 23.9 KB | 6 ms |
| **Opus 24 kbps (WB)** | 16 kHz | 9559 µs · **47%** | 1756 µs · **8%** | 60 | 26 kbps | 41.5 KB | 23.9 KB | 6 ms |
| Opus 12 kbps (NB) | 8 kHz | 6498 µs · 32% | 1373 µs · 6% | 30 | 14 kbps | 41.5 KB | 17.1 KB | 4 ms |
| AMR-NB 12.2 kbps | 8 kHz | 7750 µs · 38% | 1826 µs · 9% | 32 | 15 kbps | **4.9 KB** | **7.4 KB** | 8 ms |

Bold row is what the boards currently run.

### What "delay" means here

The harness encodes frame *N* and immediately decodes it to output position
*N*, so the 20 ms you always spend collecting a frame is **not** counted. What
is measured is the codec's internal lookahead on top of that. Real
contribution to end-to-end latency is `20 ms + the number above`:

| codec | framing | lookahead | codec total |
|---|---|---|---|
| Opus (any rate) | 20 ms | 6 ms | **26 ms** |
| AMR-NB 12.2 | 20 ms | 8 ms | **28 ms** |
| Codec 2 3200 | 20 ms | 12 ms | **32 ms** |

Resolution is one 32-sample window: ±2 ms at 16 kHz, ±4 ms at 8 kHz. Opus's
26 ms matches its documented 25 ms, which is a good sign the method is sound.

## Multi-stream

Measured as whole workloads, mixing included, all sequentially on **one core**.

### Opus, 24 kbps wideband (the shipping config)

| workload | avg | worst | state |
|---|---|---|---|
| 1 decode | 1605 µs · 8% | 9% | 18 KB |
| 1 encode | 9283 µs · 46% | **69%** | 24 KB |
| **4 decode** | 5944 µs · **29%** | 35% | 70 KB |
| **1 encode + 3 decode** | 13818 µs · **69%** | **100%** | 77 KB |
| 5 decode | 7318 µs · 36% | 44% | 88 KB |
| 6 decode | 9115 µs · 45% | 52% | 105 KB |

### Opus, 12 kbps narrowband (for comparison)

| workload | avg | worst |
|---|---|---|
| 1 decode | 1307 µs · 6% | 8% |
| 4 decode | 5123 µs · 25% | 30% |
| 1 encode + 3 decode | 11131 µs · 55% | 63% |

### A 48 kbps fullband stream added

| workload | avg | worst | state |
|---|---|---|---|
| 1 decode, 48 kHz out | 5158 µs · 25% | 26% | 18 KB |
| 1 decode, 16 kHz out | 4779 µs · 23% | 24% | 18 KB |
| 1 enc + 3 dec + that stream at 16 kHz out | 18193 µs · 90% | **109%** | 94 KB |
| 1 enc + 3 dec + that stream at 48 kHz out | 18957 µs · 94% | **112%** | 94 KB |

Peak stack across every one of these: **27.7 KB**.

### AMR-NB, 12.2 kbps

| workload | avg | state |
|---|---|---|
| 1 decode | 1563 µs · 7% | 1 KB |
| 4 decode | 6115 µs · 30% | 7 KB |
| 1 encode | 7628 µs · 38% | 3 KB |
| 1 encode + 3 decode | 12456 µs · 62% | 8 KB |
| 1 encode + 5 decode | 15337 µs · 76% | 11 KB |

### Five-stream scenarios, measured

A 48 kbps fullband stream added to four narrowband or wideband ones. **12 kbps
is narrowband, 16 kbps is wideband** — that transition is most of the difference
between the two halves of this table.

**One core**, 48 kbps stream decoded down to 16 kHz for mixing:

| scenario | avg | worst | state |
|---|---|---|---|
| 4 × 12k decode + 48k decode | 9653 µs · 48% | 52% | 88 KB |
| 4 × 16k decode + 48k decode | 10478 µs · 52% | 56% | 88 KB |
| 1 enc 12k + 3 dec 12k + 48k decode | 15152 µs · 75% | 85% | 94 KB |
| 1 enc 16k + 3 dec 16k + 48k decode | 17917 µs · 89% | **103%** | 94 KB |

Same again with the 48 kbps stream at 48 kHz output instead:

| scenario | avg | worst |
|---|---|---|
| 1 enc 12k + 3 dec 12k + 48k @ 48 kHz | 15382 µs · 76% | 86% |
| 1 enc 16k + 3 dec 16k + 48k @ 48 kHz | 18310 µs · 91% | **110%** |

**Two cores**, encoder pinned to core 0, decoders to core 1, running
concurrently:

| scenario | core 0 (encode) | core 1 (4 decodes) |
|---|---|---|
| 12 kbps | 7263 µs · 36% (worst 46%) | 10099 µs · 50% (worst 55%) |
| 16 kbps | 9978 µs · 49% (worst 68%) | 10922 µs · 54% (worst 59%) |

### The split costs about 15% extra total work

Running both cores flat out makes them contend for the same SRAM, and that is
invisible to any sum of single-core timings:

| | one core | split, both cores summed | busiest core |
|---|---|---|---|
| 12 kbps | 15152 µs | 17362 µs — **+15%** | 50% |
| 16 kbps | 17917 µs | 20900 µs — **+17%** | 54% |

So the split is not free: it buys roughly a halving of per-core load at the cost
of about 15% more total CPU. That is a very good trade here, but worth knowing
if anything else ever has to share these cores.

The same effect shows in isolation: encoding 16 kbps costs 9283–9563 µs on an
otherwise idle chip and 9978 µs while core 1 decodes.

### What this means for capacity

**4 decode fits comfortably** at 29% average, 35% worst.

**1 encode + 3 decode does not fit safely on one core.** The average is a
healthy 69%, but the worst case is exactly 100% of the frame budget. Opus
encode time varies a lot frame to frame — 46% average against 69% worst on its
own — and that variance is what consumes the margin.

**Adding a fifth 48 kbps stream:** the two decode-only cases stay comfortable on
one core (52% and 56% worst). Both encode cases need care — 12 kbps runs at 85%
worst, which works but leaves nothing spare, and 16 kbps hits 103% worst, which
does not fit at all.

**The fix throughout is the two-core split**, and it is now measured rather than
assumed. Every one of these workloads lands near 50% per core with the encoder
on core 0 and the decoders on core 1. Note that a task using Opus must stay
pinned, since Xtensa coprocessor state does not migrate.

**Decoding a 48 kbps fullband stream down to 16 kHz saves almost nothing** —
23% against 25% on its own, and in the five-stream scenarios the two output
choices differ by only 1-2 points. The cost is the fullband decode itself
(960 samples per frame rather than 320), not the resampling. So the choice
between a 16 kHz and a 48 kHz output path is architectural, not a CPU question.

**One caveat on those 48 kHz rows.** The benchmark decodes each stream and mixes
it into a buffer, but it does not rate-convert: mixing a 48 kHz stream with
16 kHz ones for real needs the others upsampled 3:1, and that work is not
measured anywhere here. If you go with a 48 kHz output path, budget for four
upsamplers on top of these figures.

**Stack:** 27.7 KB peak with several codec instances in one task. The audio
tasks are currently 32 KB, which leaves only 4 KB of margin — raise them to
40 KB before running multiple streams for real.

**Memory:** four Opus decoders plus an encoder is 77 KB, six decoders 105 KB,
against roughly 341 KB free. It fits, but the same workload in AMR-NB is 8 KB.

## Three things these numbers overturned

**Wideband is not free.** An earlier conclusion that "sample rate barely
matters" came from comparing 16 kHz against 8 kHz input *both at 12 kbps* — and
at 12 kbps Opus encodes narrowband either way, so that was narrowband against
narrowband. Real wideband costs 47% against narrowband's 32–34%. Note also that
16 kbps and 24 kbps cost the same 47%: **bandwidth is what costs, not bitrate.**

**Codec 2 is the memory hog, not the lean one.** 62.4 KB against Opus's 41.5 KB,
mostly FFT configuration in two instances. Its decode is also as expensive as
its encode (27% vs 28%), which is unusual — most codecs decode far cheaper.

**AMR-NB is not a CPU win.** At equal bandwidth it loses to Opus (38% vs 32%
encode). Its real advantage is memory: 4.9 KB of state against 41.5 KB, and 7.4 KB
of stack against 23.9 KB. That is an 8× difference, and it is what would matter
if many streams ever had to live on one chip.

## Link, measured live

- Jitter buffer held **60 ms** exactly — `min=60 max=60` over minutes.
- Zero underruns, drops, CRC errors or lost frames.
- Wire load at 24 kbps Opus: 3300 B/s, **29%** of the 115200 UART.

## End-to-end latency budget

For the shipping configuration. Marked by how each term is known:

| stage | ms | how |
|---|---|---|
| I2S input, one DMA descriptor | 20 | configured |
| encode | 10 | **measured** |
| codec lookahead | 6 | **measured** |
| UART, 66 bytes at 115200 | 6 | computed |
| jitter buffer | 60 | **measured** |
| decode | 2 | **measured** |
| I2S output DMA | 20–120 | **configured capacity, not measured** |
| **total** | **124–224** | |

The output DMA is both the largest term and the only one not pinned down. The
receiver allocates 6 descriptors of 320 samples, so up to 120 ms can sit in it,
and because `i2s_channel_write` blocks only when the buffer is full, the steady
state likely runs near that. Dropping `dma_desc_num` from 6 to 3 on the receiver
would cut up to 60 ms at the cost of tolerance to scheduling jitter.

If latency matters, that is the first thing to measure and the first thing to
trim — ahead of anything codec-related, since the whole codec contribution is
26 ms against 60 ms of jitter buffer and up to 120 ms of output DMA.
