# Live audio smoke test: WM8960 HAT, three units

**Status:** task, 2026-09-29. **Follows:**
[`c2_encode_test.md`](c2_encode_test.md) (on-device encode, ~100 ms first
sample to peer DIO1). **Supersedes** the CHANGELOG's "Next #1" plan of an
I2S amp/DAC breakout: the audio front end is now a WM8960 codec board.

**Goal:** a person talks into one unit's mic and the other two hear it in
headphones, live, on all three bench units. This is the first time the
whole voice path runs end to end:
mic → I2S → Codec 2 encode → TDMA → decode → mix → I2S → headphone.

**Scope:** Codec 2 3200 only (`CONFIG_SOAK_C2_ENCODE` stays test-only; LGPL
licensing still open). Regular 3.3 V nRF5340 DKs only. No LC3, no Audio DK,
no VPU / VOX, no engine change.

## Hardware

Three **BFab WM8960 Audio HAT** boards (Raspberry Pi 40-pin form factor).
They appear to be clones of the **Waveshare WM8960 Audio HAT**; confirm the
silkscreen matches before relying on these:

- Wiki (pin table, FAQ): https://www.waveshare.com/wiki/WM8960_Audio_HAT
- Schematic: https://files.waveshare.com/upload/f/fa/WM8960_Audio_HAT_Schematic.pdf
- WM8960 datasheet: https://files.waveshare.com/upload/1/18/WM8960_v4.2.pdf

Facts that matter here:

| Item | Value |
|---|---|
| Supply | 5 V in (DK 5 V pin), 3.3 V logic: matches the DK, no level shifting |
| Control | I2C, WM8960 at 0x1A. Pi header SDA = BCM2, SCL = BCM3 |
| I2S | BCLK = BCM18, LRCLK = BCM19, ADC data (codec → nRF) = BCM20, DAC data (nRF → codec) = BCM21 |
| MCLK | onboard 24 MHz crystal; MCLK is **not** on the header |
| Mics | two onboard MEMS mics (L/R). **No external mic input**, not even on the jack |
| Output | 4-pole OMTP 3.5 mm jack; plain 3-pole headphones are fine. Speaker terminals unused |
| Button | BCM17, unused |

## Pin map (same on both unit types)

Checked against `boards/nrf5340dk_nrf5340_cpuapp_tft.overlay`,
`boards/nrf5340dk_nrf5340_cpuapp_shield.overlay`, the radio overlay
(P1.00–P1.09) and the nRF5340 pin spec (no low-frequency-only pins on
this part). Eight jumpers, HAT 40-pin header to the DK's P0 GPIO headers:

| Signal | HAT pin (phys / BCM) | nRF5340 | nRF peripheral |
|---|---|---|---|
| 5 V | 2 / 5V | DK 5 V | — |
| GND | 6 / GND | DK GND | — |
| SDA | 3 / BCM2 | P0.25 | `i2c1` SDA |
| SCL | 5 / BCM3 | P0.26 | `i2c1` SCL |
| BCLK | 12 / BCM18 | P0.04 | `i2s0` SCK (input, slave) |
| LRCLK | 35 / BCM19 | P0.05 | `i2s0` LRCK (input, slave) |
| ADCDAT (codec → nRF) | 38 / BCM20 | P0.06 | `i2s0` SDIN |
| DACDAT (nRF → codec) | 40 / BCM21 | P0.11 | `i2s0` SDOUT |

- **Shield unit:** `i2c1` on P0.25/P0.26 already exists for the OLED
  (0x3C); the WM8960 (0x1A) joins that bus. The HAT likely carries its own
  I2C pull-ups; check the schematic, they help the OLED's rise time.
- **TFT unit:** enable `i2c1` on the same two pins. The TFT unit uses
  `spi4`, the radio uses `spi2`; serial-box 1 (`spi1`/`i2c1`/`uart1`) is
  free on both units.
- `i2s0` is its own peripheral on the nRF5340; MCK is unused (the HAT has
  its own crystal). Any GPIO works for I2S on this part.
- Kept clear of: radio P1.00–P1.09; TFT bus P0.10, P0.13–18, P0.28–31;
  shield SD P1.11–15, OLED RST P0.07; DK buttons P0.08/09/23/24; VCOM UART
  P0.19–22.
- **Agent:** verify each pin is on the DK's P0 header and not eaten by a
  solder-bridge default, then record the final map in the README and
  CHANGELOG.

## Design decisions (made; revisit only if a gate fails)

1. **WM8960 is the I2S clock master; nRF I2S is the slave.** MCLK comes
   from the HAT's crystal and is not on the header, so the nRF cannot be
   master without rework. The WM8960 PLL derives SYSCLK from 24 MHz
   (e.g. 12.288 MHz), then ADC/DAC dividers give **8 kHz, 16-bit**. Work out
   the PLL/divider registers from the datasheet.
2. **The audio clock free-runs against the TDMA frame.** Locking the audio
   clock to the frame (CHANGELOG plan, HFCLKAUDIO) is deferred. Crystal
   offsets of tens of ppm are ~1 sample every few seconds at 8 kHz.
   - **Capture side:** the encoder takes frames from an I2S capture ring;
     chunks stay keyed to the TX boundary exactly as in the simulated mic
     (a late chunk is dropped, never delayed). Occasional sample slip
     against the frame is acceptable.
   - **Playout side:** an elastic buffer. Hold its fill between two
     watermarks by dropping or repeating one sample. Count every
     correction.
3. **Capture one mic (left), mono.** Drop the right channel in the ISR or
   configure the codec for mono ADC.
4. **Receive mix:** decode each peer's chunk (two peers with three units)
   into its own 80 ms buffer, sum with saturation, feed playout. A missing
   or bad chunk plays as silence for that peer only. Keep the 8-byte test
   header (magic 0xC2, chunk index) for loss accounting; it is what makes
   card #23's garbled first packet rejectable.
5. **No sidetone** on the smoke test (the talker does not hear themselves).
   A local loopback mode exists only for gate G1.
6. **Headphones on every unit.** Speaker output stays muted, so there is
   no acoustic path from a unit's output back into its own mic.

## Gates, in order (Fail Fast)

Stop at the first gate that fails and report; do not polish later gates.

**G0 — CPU budget (no HAT needed; do it first).** Build Codec 2 decode
(mode 3200) under the same flags as encode (128 MHz, `-O2`,
`-fsingle-precision-constant`). Time decode of one 80 ms chunk on the
device, using the existing clips.
- Budget per 80 ms frame on each unit: 1 encode (~28 ms) + 2 decodes
  (three units) + mix + I2S. **Report the 4-unit case too (3 decodes).**
- Pass: total ≤ ~70 % of the app core in the 4-unit case. If it fails,
  stop and report the numbers; the fix (CMSIS-DSP FFT, the remaining
  double calls) is a separate decision.

**G1 — Codec bring-up on one DK.** I2C init, I2S slave at 8 kHz, local
loopback mic → headphone.
- Pass: clean, intelligible speech in the headphones; no clicks, buzz or
  pitch error; the measured LRCLK is 8.000 kHz (± crystal).
- If the onboard mics are unusably noisy, report it; do not rework the
  HAT.

**G2 — Live mic replaces the simulated mic.** New PCM source from the I2S
capture ring, feeding the existing per-frame encoder. `rtt_link.py mode`
selects it (e.g. `mode mic`), alongside `pcm`.
- Pass: the phase setting that was clean with the simulated mic (12 ms)
  is still clean (0 stale, staging lead ≥ 2.45 ms margin as before), and a
  peer's reconstructed WAV (`reconstruct_c2.py`) is the talker's speech.

**G3 — Decode + playout, three units.** Each unit decodes both peers,
mixes, and plays. One talker at a time, talkers in different rooms or
far apart so the other units' mics do not pick up the talker directly.
- **Run:** 5 min continuous, every unit talked into in turn.
- **Pass:**
  - Speech from each unit intelligible on both others. No periodic
    clicks, dropouts, or pitch wobble. (Subjective; the engineer is the
    judge.)
  - Playout underruns and overruns 0 after the start-up fill.
  - Elastic-buffer corrections: count and rate logged, and consistent
    with the two crystals' offset (a few per minute is fine; a steady
    stream is a bug).
  - Missing / stale chunks 0 apart from card #23's first packet.
  - Radio counters unchanged from the encode test (PER 0, `stale_retx` 0,
    `slot_timeouts` 0).

**G4 — Mouth-to-ear latency (measure, do not gate).** First sample
captured on the talker to that sample leaving the listener's DAC.
Use the existing common-view clock offset (`reconstruct_c2.py --tx`, needs
three units) plus a playout timestamp, or a clap recorded on both ends.
Expected ≈ 100 ms (to peer DIO1) + decode + playout buffer. Report the
number and the breakdown.

## Telemetry to add (RTT `status` + records)

- Per-unit CPU: encode, decode per peer, mix, worst frame total.
- Capture ring: overruns. Playout: fill level min/max, underruns,
  overruns, drop/repeat corrections.
- Per peer: chunks decoded, missing, bad header.
- Keep the TX path without RTT instruction-identical where it is today,
  as the previous tasks did; say so in the CHANGELOG if that changes.

## Deliverables

- Firmware on `TDMA_shim`, both unit types building (`-p always`).
- Pin map and build lines in the README.
- CHANGELOG entry with the gate results in the house style: numbers,
  what passed, what failed, what is open.
- Update the CHANGELOG's "Where things stand" and "Next" list.
- If a gate fails, the write-up of that gate's numbers is the deliverable.
