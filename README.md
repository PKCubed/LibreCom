# LibreCom
**An Open-Source, Long-Range, Sub-GHz Digital Intercom System**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![License: CERN-OHL-S](https://img.shields.io/badge/License-CERN--OHL--S-orange.svg)](https://ohwr.org/cern_ohl_s_v2.txt)

Welcome to the **LibreCom** repository. This project aims to build a fully open-source, digital wireless intercom system designed for live event production, theater crews, and outdoor decentralized communication (like skiing or hiking). 

By combining the wall-penetrating power of **Sub-GHz radios**, the ultra-low bandwidth of the **Codec 2 vocoder**, and a custom **TDMA (Time-Division Multiple Access)** protocol, this system supports multiple simultaneous talkers and infinite listeners over distances that standard 2.4GHz/Wi-Fi systems cannot reach.

---

## System Feature Goals

### Overall System
* **Low Latency:** Latency should be low enough such that conversations can be made quickly. Ideally, when broadcast to the hub and back to the person talking, the latency is low enough that it doesn't "speach jam" the person talking. Alternatively, some method of removing the person talking from the feed they receive back may need to be implemented.

### The Belt Packs (End-User Nodes)
The belt packs are designed to be cheap, rugged, and feature-rich for live-event professionals.
* **Standard Headset Support:** Uses universal TRRS (3.5mm) headsets (CTIA/OMTP).
* **Local IEM Mixing:** Features an external stereo input jack. You can plug in an In-Ear Monitor (IEM) pack or other device, and the belt pack will mix this audio locally with the coms feed fed into the headset.
* **Spatial Panning:** Comm audio can be software-panned entirely to the Left or Right ear, allowing users to separate crew chatter from program audio.
* **Physical Controls:**
  * Volume Knob
  * Push-To-Talk (PTT) Button
  * Push-To-Mute Button (Configurable to mute either the mic, the comm feed, or the local IEM feed)
* **VOX Support:** Voice Operated Exchange for hands-free operation without needing the PTT button.
* **Multiple Talk Groups:** More than one conversation space for separating teams like video and stage crews while keeping the ability to broadcast to specific or all talk groups.

### The Base Station (Central Matrix)
For structured events (theater, concerts), the system relies on a Base Station to act as the TDMA master clock and audio routing matrix.
* **Program Audio Injection:** Jacks to input external audio (like a Front-of-House stage mix) to be broadcast to all belt packs.
* **Hardware Outputs:** Multiple physical audio outputs on the base station that can be routed in software (e.g., routing "Talk Group A" to Output 1, and "Talk Group B" to Output 2 for the stage manager's console).
* **Web UI Management:** The base station hosts a local web server interface. Stage managers can connect via phone/laptop to change user volumes, assign talk groups, configure priorities, and monitor battery levels.

### Decentralized "Mesh" Mode
No base station? No problem. The belt packs can be configured to operate in a completely decentralized mode. Perfect for outdoor sports (skiing, hiking, airsoft) where a group of friends can communicate directly point-to-point like highly advanced digital walkie-talkies.

---

## Hardware Architecture

The hardware is designed to decouple the heavy digital signal processing from the strict timing requirements of the radio link.

* **Belt Pack Main Processor (MCU):** Raspberry Pi RP2350. These dual-core chips feature hardware DSP instructions to easily crunch the Codec 2 voice compression math.
* **Radio Transceiver:** RFM69HCW running in the 915 MHz (US) / 868 MHz (EU) ISM bands. Offloading the radio to an SPI peripheral ensures pristine timing for the TDMA frames.
* **Audio Frontend:** I2S Audio DAC and ADC IC(s) with amplification for both headphones and for a microphone.
* **The Hub:** Raspberry Pi Hat with RFM69HCW, I2S ADCs and DACs for physical audio IO, and some kind of physical user interface for quick adjustments to the system's configuration

---

## Project Roadmap

- [ ] **Phase 1: Proof of Concept**
  - Connect MCU to I2S ADC/DAC and establish I2S audio pass-through.
  - Implement Codec 2 encoding/decoding loopback in firmware.
- [ ] **Phase 2: The Radio Link**
  - Connect the SPI RFM69 modules.
  - Successfully transmit Codec 2 payloads point-to-point (Simplex).
- [ ] **Phase 3: The TDMA Matrix**
  - Implement the Base Station TDMA master clock.
  - Achieve 4 simultaneous talkers and 1 mixed broadcast downlink.
- [ ] **Phase 4: Hardware Prototyping**
  - Design custom V1 PCBs integrating the MCU, Radio, Codec, and physical inputs (PTT, Knobs, IEM jacks).
- [ ] **Phase 5: UI & Polish**
  - Build the Base Station Web UI.
  - Implement VOX, Panning, and User Profiles.

---

## ⚖️ Licensing

To ensure this project remains open and beneficial to the community while allowing for commercial manufacturing:

* **Software / Firmware:** Licensed under the **GNU GPLv3**. *(Note: This complies with the underlying LGPL license of the Codec 2 library).*
* **Hardware / PCBs:** Licensed under the **CERN-OHL-S** (Strongly Reciprocal). 

You are free to build, use, and sell this hardware. However, if you modify the circuit board designs or the firmware and distribute the resulting product, you must share your modifications back with the community under the same open licenses.

---
*Built by the open-source live production community.*
