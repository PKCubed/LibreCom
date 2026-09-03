# LibreCom bring-up

Audio path: PCM1808 → I2S → Codec 2 (mode 3200) → framed UART → Codec 2 decode
→ I2S → PCM5102A.

## Wiring

**UART link** — this is the only connection between the two boards:

| Transmitter | Receiver |
|---|---|
| GPIO4 (TX) | GPIO5 (RX) |
| GND | GND |

The ground wire is not optional. Two boards on separate USB ports usually share
ground through the host, but "usually" is how you get an hour of confusing
bit errors.

**Give each board its own supply.** Do not power one board from the other
through jumper wires. The droop is enough to trip the brownout detector, and
the symptom is not an obvious power failure — it is `bytes=0` on a link whose
firmware is provably correct on both ends. This cost a full debugging session
once already.

**Transmitter → PCM1808**

| ESP32-S3 | PCM1808 |
|---|---|
| GPIO16 | SCKI |
| GPIO17 | BCK |
| GPIO18 | LRCK |
| GPIO19 | DOUT |

**Receiver → PCM5102A**

| ESP32-S3 | PCM5102A |
|---|---|
| GPIO17 | BCK |
| GPIO18 | LRCK (WS) |
| GPIO19 | DIN |
| — | SCK tied to GND (internal PLL) |

## Build

Settings that must match on both boards live in
`components/audio_link/include/link_config.h`. Change them there, then rebuild
and reflash **both**.

```bash
idf.py build flash monitor
```

### Always confirm what is actually on the board

Both sides print an identical-looking banner at boot, and they must agree:

```
tx: sending   mu-law PCM, 160 byte payload, 50 frames/s, 8200 bytes/s at 115200 baud
rx: expecting mu-law PCM, 160 byte payload, 50 frames/s, 8200 bytes/s at 115200 baud
```

If those two lines disagree, stop and reflash — nothing downstream will make
sense. This is worth checking every time, because a baud mismatch does not
produce garbage audio, it produces **complete silence with `bytes=0`**: the
UART driver takes its framing-error path instead of its data path, never drains
the FIFO, and the overflow handler then resets it. Zero bytes are delivered.

You can also check a built image without flashing it. Exactly one of these
strings is compiled in, so this tells you what a binary really is:

```bash
grep -ao "mu-law PCM\|16-bit PCM\|Codec 2 mode 3200" build/receiver.bin
```

## The stages

Each stage adds exactly one thing. Do not skip ahead: if stage 3 misbehaves
and you have not run stage 1, you have three suspects instead of one.

### Stage 0 — is the DAC alive?

`receiver/main/main.c`: `#define RX_LOCAL_TONE 1`

Receiver only. Ignores the UART entirely and plays a 440 Hz tone it generates
itself. **Expect:** a clean, steady A4 on both channels. If this fails, nothing
downstream matters — it is wiring, the PCM5102A strapping, or the I2S pins.

Set it back to `0` before moving on.

### Stage 1 — does the wire work?

- `link_config.h`: `#define LINK_SEND_PCM 1`
- `transmitter/main/main.c`: `#define TX_SOURCE TX_SRC_SINE`

Codec 2 and the ADC are both out of the picture. The transmitter synthesises a
440 Hz tone, compands it to 8-bit µ-law, and the receiver plays it.

**Expect:** the same clean tone as stage 0, after roughly 60 ms of startup
delay. This is the stage that proves framing, the jitter buffer and clock-drift
handling all work.

µ-law is deliberate. It keeps the payload at 160 bytes so this stage runs at
**the same 115200 baud as the Codec 2 stage** — a bring-up step should never be
electrically harder than the thing it is a stepping stone to. It costs about
39 dB SNR, which you will not hear on a test tone.

If you specifically want to test raw 16-bit PCM, set `LINK_PCM_ULAW 0`. That
gives a 324-byte frame at 460800 baud, and is worth knowing about: a 324-byte
frame is 27x more likely to catch a bit error than a 12-byte one. At a bit
error rate of 1e-3, 12-byte Codec 2 frames still get through 90% of the time
while 324-byte frames get through 8% of the time — which sounds like complete
silence, not like noise. A marginal wire that carries Codec 2 perfectly can
drop essentially every linear-PCM frame.

### Stage 2 — can Codec 2 keep up?

- `link_config.h`: `#define LINK_SEND_PCM 0`
- transmitter still on `TX_SRC_SINE`

**Judge this stage by the log, not by ear.** Codec 2 is a speech vocoder; a
pure sine is exactly the signal it is worst at, and it will sound buzzy and
warbly even when everything is working correctly. What matters is that the
encoder finishes each frame inside its 20 ms budget and no frames are lost.

### Stage 3 — does the ADC work?

- `link_config.h`: `#define LINK_SEND_PCM 1`
- transmitter: `#define TX_SOURCE TX_SRC_ADC`

Real audio, no Codec 2. The code assumes the PCM1808 is strapped as an I2S
**slave** (MD0 = MD1 = 0), with the ESP32 supplying SCKI, BCK and LRCK.

**With nothing connected to the analog input yet, this stage is still worth
running** — it tells you whether the I2S bus is alive, which is the part most
likely to be wrong:

- `peak=0` exactly, every second, means no bits are arriving. That is an I2S
  problem, not a signal problem. In order: the GPIO19 caveat below, then DOUT
  wiring, then `ADC_MCLK_MULT = I2S_MCLK_MULTIPLE_512`.
- A small wandering `peak` (tens to a few hundred) is the ADC noise floor.
  That is the healthy answer, and it means the bus is working.
- `peak` pinned near 32767 means the input is railed or the slot alignment is
  wrong.

With a floating input, touching the analog input pin with a finger should make
`peak` jump noticeably from mains hum. That is a complete end-to-end proof of
the ADC path without any signal source at all.

Once you have something to feed it, watch `peak` / `rms` track the audio.

#### The GPIO19 caveat

On the ESP32-S3, **GPIO19 is USB D-** and GPIO20 is USB D+ (`USBPHY_DM_NUM` /
`USBPHY_DP_NUM` in `soc/usb_pins.h`). These builds have
`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, so the internal USB PHY is
initialised and holds both pins.

The receiver gets away with GPIO19 because it drives it as an I2S *output* and
the GPIO matrix wins. The transmitter uses GPIO19 as an I2S *input*, where the
USB PHY can hold the pin and swamp whatever the PCM1808 is driving. Symptom:
`peak=0` or a `peak` frozen at some constant.

Two fixes, either is fine:

- **Move the pin.** Set `I2S_DIN_IO` to a free GPIO — 6, 7, 8, 15 and 21 are
  all clean on a DevKitC-1 — and move that one jumper. This is the better fix
  and keeps USB available.
- **Free the pins in software.** Set `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` in
  `sdkconfig.defaults` so the USB PHY is never brought up. No rewiring, but you
  lose the native USB port as a console.

Avoid GPIO26-32 entirely on this chip; they belong to the SPI flash.

### Stage 4 — the whole thing

- `link_config.h`: `#define LINK_SEND_PCM 0`
- transmitter: `TX_SRC_ADC`

Speech in, speech out. Codec 2 at 3200 bps sounds robotic — that is the codec
doing its job at 3.2 kbit/s, not a bug.

## Reading the logs

Both boards print one line per second. Rate fields (`frames`, `wr`, `bytes`,
`good`) are normalised to a true per-second figure, because the reporting window
is a whole number of 20 ms frames and so overshoots 1 s slightly. A healthy link
reads exactly 50 and 600. Event counters (`crc`, `lost`, `under`, `drop`, `plc`)
are raw counts for the window.

**Transmitter**

```
frames=50 peak=10302 rms=2541 dc=-3 | wr=600/600 werr=0 | enc us avg=5755 max=6871 | i2s wait ms=707 | heap=320380
```

| Field | Healthy | What it means if it is not |
|---|---|---|
| `frames` | exactly 50 | Not 50 means the loop is not running at real time. |
| `wr` | the two numbers match | Bytes the UART driver accepted, against what we tried to send. Matching here plus `bytes=0` on the receiver proves the data left this chip and the fault is the pin, the jumper, or the far end. |
| `werr` | 0 | Rejected or short writes. Non-zero means the TX ring is backing up. |
| `peak` / `rms` | peak a few thousand on speech | `peak=0` means the ADC is delivering nothing. `peak=32767` constantly means clipping — drop `ADC_GAIN`. |
| `dc` | within ±50 | A large constant offset means the DC blocker is off or the input is biased. |
| `enc us avg` | ~5800 in mode 3200 | Measured 5755 avg / 6871 max, i.e. 29% of the 20 ms frame budget. Must stay well under 20000; above that the encoder cannot keep up. |
| `i2s wait ms` | 700–950 | This is idle time. Near zero means no headroom left. |

**Receiver**

```
bytes=600/600 rxq=11 good=50 crc=0 lost=0 | jb ms avg=60 min=60 max=60 | under=0 drop=0 plc=0 | dec us avg=5777 max=9195 | heap=303772
```

| Field | Healthy | What it means if it is not |
|---|---|---|
| `bytes` | the two numbers match | Raw bytes received per second, against what the sender should be producing. **This is the field to read first when nothing comes out.** |
| `rxq` | a few hundred | Peak UART driver backlog. Creeping toward 4096 means the reader is not keeping up and bytes are being lost. |
| `good` | exactly 50 | Fewer means frames are not arriving. |
| `crc` | 0 | Non-zero means corruption on the wire: check the ground wire, shorten the jumpers, or drop the baud rate. |
| `lost` | 0 | Frames the sender emitted that never arrived. |
| `jb ms avg` | near 60 | This is the jitter buffer depth, and it is also your latency. |
| `under` | 0 | The buffer ran dry. A few at startup are normal; a steady stream means the sender is slower than this board. |
| `drop` | 0, or one every few minutes | Trimming for clock drift. Steady drops mean the sender is faster than this board. |
| `plc` | 0 | Frames papered over by repeating the previous one. |
| `dec us avg` | ~5800 in mode 3200 | Decode costs about the same as encode, not less: mode 3200 synthesis runs an inverse FFT per 10 ms subframe plus the postfilter. 29% of the 20 ms budget. |

### When nothing comes out of the DAC

Read `bytes` first — it splits the problem in half immediately:

| `bytes` | `good` | Diagnosis |
|---|---|---|
| 0 | 0 | Nothing on the wire. Wiring, ground, or the sender is not running. |
| matches expected | 0, `crc` climbing fast | Bytes are arriving but no frame survives. Line errors, or the two sides are built with different `link_config.h` settings. Compare the boot banners: both print payload size, bytes/s and baud. |
| matches expected | 50, but silence | The link is fine and the problem is downstream — check `under`, `jb ms`, and then the DAC with stage 0. |
| well below expected | low | The sender is not keeping up, or the baud rates disagree. |
| well above expected | 0 | Baud mismatch: the receiver is sampling a slower line too fast and seeing each byte several times. |

### If `bytes=0`: work outwards from the chip

Take these in order — each one eliminates a layer.

0. **Confirm each board has its own power supply.** Powering one board from the
   other through the jumpers browns it out, and it looks exactly like a link
   fault. Check this before touching any firmware.
1. **Compare the two banners.** They must agree on payload, bytes/s and baud.
2. **Check `wr=` on the transmitter.** If it matches, the bytes reached the UART
   driver and nothing above the pin is at fault.
3. **Run the receiver loopback** (below). That splits receiver from
   transmitter-plus-cable.
4. **Try the other jumper.** Set `LINK_SWAP_PINS 1` in `link_config.h` and
   reflash both. You have two wires cross-connected 4 to 5 and the audio only
   uses one of them; this moves it onto the other. If the link comes alive, the
   first wire or one of its crimps is dead.

### Is it this board, or the other one?

`receiver/main/main.c`: `#define RX_UART_LOOPBACK 1`

This loops the receiver UART1 TX back into its own RX inside the chip and feeds
it locally generated frames at the real 20 ms cadence. The cable is bypassed
entirely.

- **Loopback gives `good=50`** (and in the PCM modes, a clean 440 Hz tone):
  this board UART, the framing, the jitter buffer and the DAC are all proven.
  The fault is the transmitter or the cable.
- **Loopback also gives `bytes=0`**: the problem is on this board — UART pins,
  driver install, or the parser.

Set it back to `0` afterwards.

If bytes are arriving but nothing syncs, the receiver dumps the first 16 raw
bytes once a second. A frame should start `A5 5A`. If you can see `A5 5A` in
there, the two sides disagree about payload length; if it looks like noise, the
baud rates disagree.

A `drop` or `under` every several minutes is normal and expected — the two
crystals are not identical and something has to give. A steady stream of either
means a real rate mismatch, not drift.

## Knobs, if a stage misbehaves

In `transmitter/main/main.c`:

- `ADC_GAIN` — raise it if `peak` is small but non-zero.
- `ADC_USE_RIGHT` — set to 1 if the PCM1808 is delivering audio on the right
  slot instead of the left.
- `ADC_MCLK_MULT` — `I2S_MCLK_MULTIPLE_512` gives the PCM1808 a 4.096 MHz
  system clock instead of 2.048 MHz. At 8 kHz the 256x setting sits exactly on
  the part's documented minimum, so this is the first thing to try if the ADC
  reads zero or garbage.
- `ADC_DECIM` — run the I2S bus at 2/4/6 times 8 kHz and filter back down in
  software. Use this if the PCM1808 simply will not run at 8 kHz. `ADC_DECIM 6`
  puts the bus at 48 kHz, which is comfortable territory for that part.

In `receiver/main/main.c`:

- `JB_TARGET_MS` — raise it if you see underruns, lower it for less latency.
- `RX_UART_LOOPBACK` — bypass the cable to test this board on its own.

In `link_config.h`:

- `LINK_SWAP_PINS` — move the audio link onto the other of the two jumpers.
- `LINK_PCM_ULAW` — 1 keeps the raw-PCM stage at 115200 with a 160-byte
  payload. 0 gives 16-bit linear, which needs a 320-byte payload at 460800.
- `LINK_UART_BAUD` — derived from the mode above. If you see CRC errors, the
  fix is a shorter or better-grounded jumper rather than a lower baud: the
  Codec 2 path only needs about 6 kbit/s and already has 19x headroom at
  115200.
