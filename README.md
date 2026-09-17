# LibreCom
**An Open-Source, Long-Range, Sub-GHz Digital Intercom System**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![License: CERN-OHL-S](https://img.shields.io/badge/License-CERN--OHL--S-orange.svg)](https://ohwr.org/cern_ohl_s_v2.txt)

Welcome to the **LibreCom** repository. This project aims to build a fully open-source, digital wireless intercom system designed for live event production, theater crews, and other communication applications.

By combining the wall-penetrating power of **Sub-GHz radios**, high quality low bandwidth OPUS audio compression, and a custom **TDMA (Time-Division Multiple Access)** protocol, this system supports multiple simultaneous talkers and infinite listeners over distances that standard 2.4GHz/Wi-Fi systems cannot reach.

---

## System Feature Goals

### Overall System
* **Low Latency:** Latency should be low enough such that conversations can be made quickly.
* **Flexibility:** Each belt pack should support any standard TRRS headset. The hub should allow analog connections as well to interface with other systems.

### The Belt Packs (End-User Nodes)
The belt packs are designed to be cheap, rugged, and feature-rich for live-event professionals.
* **Standard Headset Support:** Uses universal TRRS (3.5mm) headsets (CTIA).
* **Local IEM Mixing:** Features an external stereo input jack. You can plug in an In-Ear Monitor (IEM) pack or other device, and the belt pack will mix this audio locally with the coms feed fed into the headset.
* **Spatial Panning:** Comm audio can be software-panned entirely to the Left or Right ear, allowing users to separate crew chatter from program audio.
* **Audio Ducking** User's can configure the belt packs to duck other audio making way for priority talkers.
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

### Decentralized "Mesh" Mode (stretch goal)
No base station? No problem. The belt packs can be configured to operate in a completely decentralized mode. Perfect for outdoor sports (skiing, hiking, airsoft) where a group of friends can communicate directly point-to-point like highly advanced digital walkie-talkies.

---

## Hardware Architecture

The hardware is designed to decouple the heavy digital signal processing from the strict timing requirements of the radio link.

* **Belt Pack Main Processor (MCU):** ESP32-S3
* **Radio Transceiver:** SX1262
* **Audio Frontend:** I2S Audio DAC and ADC IC with amplification for both headphones and for a microphone.
* **The Hub:** Raspberry Pi Hat with custom hat including SX1262, I2S ADCs and DACs for physical audio IO, and some kind of physical user interface for quick adjustments to the system's configuration

---

## Project Roadmap

- [ ] **Phase 1: Proof of Concept**
  - Connect MCU to I2S ADC/DAC and establish I2S and internal analog audio pass-through.
  - Implement OPUS encoding/decoding loopback in firmware, then link two belt packs via wires to transmit audio.
- [ ] **Phase 2: The Radio Link**
  - Connect the sx1262
  - Successfully transmit OPUS audio in one direction
- [ ] **Phase 3: The TDMA Matrix**
  - Implement the Base Station TDMA master.
  - Achieve 4 simultaneous talkers with the stretch goal of a larger additional 48kbps OPUS stream.
- [ ] **Phase 4: Hardware Prototyping**
  - Design custom PCBs integrating the MCU, Radio, Codec, and physical inputs (PTT, Knobs, IEM jacks).
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
