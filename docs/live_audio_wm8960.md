# Live audio smoke test: WM8960 HAT, three units

**Status:** task, 2026-09-29. **G0 passed after Codec 2 speed fixes; G1 and
G2 passed (2026-09-29)**, results at the end; G3–G4 not started. **Follows:**
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
3. **Capture both mics; encode a mono mix.** *(Revised after G1; was
   "capture one mic (left), mono".)* The capture ring keeps left and right;
   the encoder gets left, right, or their average (the default), selectable
   at run time. Both channels stay available for noise suppression or
   beamforming later.
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

## G0 result (2026-09-29): FAIL — stopped here

Codec 2 decode is built under the encode flags (128 MHz, `-O2`,
`-fsingle-precision-constant`) in `src/tdma_console/c2_dec.c`:
- one persistent state per peer slot;
- a saturating mix of each decoded chunk;
- whole-CPU accounting from Zephyr's thread runtime stats (DWT-timed);
- an isolated benchmark (`rtt_link.py bench N`).

**Isolated, 100 chunks of the loaded PCM clip, per 80 ms chunk, all three
units:**

| | min | avg | p99 / max |
|---|---|---|---|
| Encode | 25.2–26.6 ms | 25.8–27.3 ms | 26.6–28.3 ms |
| **Decode** | 18.9–22.0 ms | **20.5–23.2 ms** | 23.2–26.4 ms |
| Mix | | | 0.07–0.11 ms |

One Codec 2 state takes **31.3 KB** of heap (largest-free-block probe).

**Budget per 80 ms frame (averages):**

| Kit | Work | Time | Share of the core |
|---|---|---|---|
| 3 units | 1 encode + 2 decodes | ~72 ms | ~90 % |
| 4 units | 1 encode + 3 decodes | ~95 ms | ~119 % (cannot keep up) |
| Gate | | ≤ 56 ms | ≤ 70 % |

That is before I2S, playout, radio and UI.

**In situ, three units, phase 12 ms, 1 min** (encode + both peers decoded
live):
- **Whole-CPU busy 88.6–91.8 %:** encoder 31.5–32.9 %, decoder
  53.5–56.0 %, radio ~0.8 %, data thread ~1 %.
- **It keeps up, just:** 0 chunks dropped. 2 rejected as not audio per
  unit, which is card #23's first packet, as designed.
- **The encoder is unaffected** because it outranks the decoder:
  capture-to-stage ≤ 7.5 ms, staging lead ≥ 4.5 ms, 0 stale.
- **Memory:** heap left after the encoder and three peer decoder states is
  5.8 KB (arena 128 KB). A fourth decode state does not fit, so the 4-unit
  case could not be run even as a phantom stream.

**Where decode time goes (from the code; not yet profiled).** Per 20 ms
frame, 3200 mode synthesises two 10 ms sub-frames. Each has:
- a 512-point FFT for the LPC-to-magnitude step;
- a 512-point inverse real FFT for synthesis;
- a `cosf`/`sinf`/`atan2f` per harmonic in phase synthesis;
- the postfilter.

The FFTs are kiss_fft. The two double-precision calls left in `sine.c`
are init-only.

**Options for the separate decision** (none taken):
1. **Profile decode first.** Time FFT vs trig vs the rest with the cycle
   counter, which is cheap. Then pick from the options below with numbers.
2. **CMSIS-DSP FFTs** (upstream's `FDV_ARM_MATH` path). Helps encode and
   decode alike; likely the biggest single gain.
3. **Decode only active talkers.** Each frame's energy is in the Codec 2
   bits, so silent peers can be skipped without decoding them. Typical
   intercom use is one talker at a time, which bounds decode at 1–2
   streams regardless of kit size. It needs a mixing and VAD policy.
4. **A lower-rate mode** (1600: 40 ms frames, half the frames per chunk,
   roughly half the CPU) at a quality cost.
5. **RAM:** 31 KB per state. The 4-unit kit's encoder + 3 decoders need
   ~125 KB of heap next to the 128 KB PCM clip buffer. That buffer goes
   away with a live mic (a capture ring is a few KB), so memory is not
   the blocker; CPU is.

### G0 profile (2026-09-29)

**Method.** `CONFIG_SOAK_C2_PROFILE` (`src/tdma_console/c2_prof.c`) leaves the
vendored codec untouched:
- The linker's `--wrap` sends the codec's cross-file calls, and its libm
  calls, to wrappers timed with the cycle counter.
- Each function gets inclusive time, the FFT and libm time spent inside
  it, and its own (exclusive) time.
- Each wrapper's own cost is measured once and subtracted per call.

The run is `rtt_link.py bench 200` on the shield unit, whose output is kept
in `soaks/rtt/g0_profile_shield.txt` (untracked). A second run matched to
within a few µs.

**Encode, 27.4 ms per 80 ms chunk:**

| Where | ms / chunk | Share |
|---|---|---|
| FFTs (8 in `dft_speech`, 8 in `nlp`; 512-point complex, kiss_fft) | 10.6 | 39 % |
| `nlp` own code | 10.2 | 37 % |
| — of which its 48-tap decimation FIR * | ~8.0 | ~29 % |
| `speech_to_uq_lsps` | 2.4 | 9 % |
| `dft_speech` own (windowing) | 2.0 | 7 % |
| pitch refinement, amplitudes, voicing, LSP quantise, glue | ~2.2 | 8 % |
| libm | 0.2 | <1 % |

\* From a temporary, uncommitted probe build with timestamps between
`nlp`'s stages. The FIR computes all 80 outputs per call, although `nlp`
keeps every fifth. It also shifts a 48-entry delay line per sample and
reads its coefficients from flash. It is all float; there is no double
maths here.

**Decode, 25.2 ms per 80 ms chunk:**

| Where | ms / chunk | Share |
|---|---|---|
| FFTs (8 real forward in `aks_to_M2`, 8 real inverse in `synthesise`, …) | 9.6 | 38 % |
| libm: `powf` + `sqrtf` per spectral bin in the LPC post-filter (inside `aks_to_M2`) | 6.3 | 25 % |
| libm: `cosf`/`sinf`/`atan2f` per harmonic in phase synthesis | 1.3 | 5 % |
| libm: other (`synthesise`, `lsp_to_lpc`) | 0.8 | 3 % |
| `aks_to_M2` own code | 4.6 | 18 % |
| `synthesise`, `phase_synth`, postfilter own; glue | ~2.2 | 9 % |

Decode makes ~3,670 libm calls per chunk, against ~84 for encode.

**Candidate fixes, with rough savings.** These are estimates to guide the
decision, not measurements.

| Fix | Kind | Encode | Decode |
|---|---|---|---|
| CMSIS-DSP FFTs (upstream `FDV_ARM_MATH`) | build, CMSIS-DSP module | −(5–8) ms | −(5–7) ms |
| Decimating FIR in `nlp` (16 outputs, circular buffer) | patch to vendored `nlp.c` | −(5–7) ms | — |
| `-fno-math-errno` (lets `sqrtf` become one instruction) | flag | — | −(0.5–1) ms |
| Cheaper `powf` in the LPC post-filter, or turn the post-filter off (`codec2_set_lpc_post_filter`, a quality cost) | patch or API | — | −(3–6) ms |
| Decode only active talkers | architecture | — | caps decodes at 1–2 per frame |

Taking the first four at the middle of their ranges gives roughly 14 ms to
encode and 14 ms to decode. That is ~56 ms per frame for four units (70 %,
right at the gate) and ~42 ms for three (~52 %). Talker gating would take
four units under 50 %. Each fix should be measured with this profiler as
it goes in.

### G0 re-run after the fixes (2026-09-29): PASS

Talker gating was ruled out: simultaneous talkers are rare but must work,
so every peer is always decoded. The fixes went in one at a time, each
measured with the profiler (per 80 ms chunk, profile build, shield):

| Step | Encode | Decode | Note |
|---|---|---|---|
| Start | 27.4 ms | 25.2 ms | |
| `-fno-math-errno` | 27.4 | 25.2 | no gain: GCC already emits `vsqrt`; reverted |
| CMSIS-DSP FFTs (`CONFIG_SOAK_C2_ARM_FFT`, upstream `FDV_ARM_MATH`) | 19.4 | 22.3 | also cuts a Codec 2 state from 31.3 to 11.9 KB of heap |
| Patch 0001: decimating `nlp` FIR | 12.2 | 22.3 | encoded frames bit-identical to upstream (372 chunks compared on the device) |
| Patch 0002a: post-filter power function instead of `powf` | 12.2 | 16.8 | max relative error 1.4e-6 (host check over 1e-30..1e30) |
| `-O3` instead of `-O2` | 12.2 | 16.8 | no gain; reverted |
| Patch 0002b: direct order-10 LPC spectra in one pass | 12.2 | 13.9 | within 3.4e-7 of NumPy's FFT relative to the peak, all bins, 300 random filters |

The patches live in `external/codec2-patches/`, and
`scripts/vendor_codec2.py` applies them. Re-vendoring from a clean
upstream checkout reproduces the tested sources exactly (ignoring line
endings).

**Isolated benchmark, normal encode images, all three units, per chunk:**
encode 12.1–12.2 ms average (13.2 max), decode 12.9–13.2 ms average
(16.8 max), mix ~0.1 ms, 12.1 KB heap per state.

**In situ, 1 min each, phase 12 ms:**

| | MASTER (shield) | SEC 1 (TFT) | SEC 2 (TFT) |
|---|---|---|---|
| 3 units (encode + 2 decodes): CPU busy | 48.6 % | 57.6 % | 57.6 % |
| 4-unit emulation (+1 phantom decode): CPU busy | **64.4 %** | 72.1 % | 71.3 % |
| 4-unit: encoder / decoder share | 14.8 / 47.5 % | 15.1 / 47.8 % | 15.1 / 47.5 % |

- **Gate met: ~64 % for four units** on the shield, whose OLED costs
  little, which is the closest to a display-less production unit. Three
  units are at ~49 %.
- **The TFT's UI takes ~9 %** once there is CPU to spare (busy minus the
  voice-path threads). It had only ~2 % when the codec threads, which
  outrank it, saturated the core. That is why the TFT units read ~8 points
  higher; production has no display.
- **Everything else held.** 0 decoder drops. Staging lead ≥ 8.1 ms at the
  12 ms phase, 0 below the deadline. Every chunk was the same over the
  air. Encoder bit-exactness against `pycodec2` was unchanged: the CMSIS
  FFTs did not flip a single extra bit. Only card #23's first packets
  were rejected as not audio.
- **The phase could come down at G2.** A frame now encodes in ≤ 3.8 ms,
  so capture-to-stage is ≤ 3.9 ms. That leaves ~8 ms of slack at the
  12 ms phase; ~8 ms would still leave margin, for about 96 ms first
  sample to peer.
- **In reserve, if the live pipeline needs more:** fast `sinf`/`cosf`/
  `atan2f` in phase synthesis and synthesis (~2 ms of libm per decode).

## G1 result (2026-09-29): PASS

Done on one TFT DK with the standalone smoke-test app
[`apps/wm8960_smoke`](../apps/wm8960_smoke/README.md), not the unit firmware.
Findings that the unit firmware (G2 onward) has to carry over:

- **The HAT has no I2C pull-ups** (a Pi supplies its own). Without any, the
  bus idles low and nothing ACKs. With the nRF's internal pull-ups the codec
  answers, but writes still fail now and then: 15 retries across one boot's
  ~30 writes. `wm8960_write()` retries up to 5 times. **Real units need
  2.2–4.7 kΩ pull-ups to 3.3 V on SDA and SCL.**
- **Clock: pass.** The codec is the I2S master; its PLL runs from the 24 MHz
  crystal (SYSCLK 12.288 MHz, ADC and DAC /6 = 8 kHz). LRCLK measured
  **7999.96–7999.97 Hz (−5 ppm)** over 31 s against the DK's 32 kHz crystal.
  0 I2S restarts or timeouts.
- **I2S words are 24-bit.** The codec's BCLK is SYSCLK / 32 = 384 kHz, 48
  clocks per frame; that holds exactly two 24-bit words.
- **The codec reads each DAC word one bit clock early.** Its MSB is the last
  bit of the previous slot. Unfixed, every zero crossing of the audio
  becomes a full-scale step: speech plays as a loud square of its sign,
  recognisable but garbled and static-filled, while a tone sounds nearly
  normal.
  - Found by a tone-level sweep through the speaker into the HAT's own mic.
    The output did not change with the digital level (−30 to −2 dBFS) and
    THD was ~−10 dB; the DAC's soft mute removed it (so not crosstalk); a
    tone on a DC offset, which never changes sign, came through clean.
  - Fix, `pack_tx()`: each word carries its sample shifted up one bit, and
    its bit 0 carries the next sample's sign. The stream is delayed one
    frame (125 µs) so the next sample is known at a block's end. After the
    fix the output follows the level (−30 → −18 dBFS: +11.7 dB) and THD is
    −25 to −39 dB.
  - The ADC direction (codec → nRF) is aligned and needs no fix.
- **Speech playback: pass.** The engineer judged the clips clean on the
  HAT's speaker after the fix. Clips band-limited to 300–3400 Hz sound
  clearly better on the small speaker than the raw ones; about half the raw
  clips' energy is below 300 Hz (`apps/wm8960_smoke/tools/speech_filter.py`).
- **Mic loopback: pass.** Mic → headphones "works really well" by ear, on
  each mic alone and on both, at Waveshare's default gains (PGA +12 dB,
  boost +29 dB). The idle floor is dominated by rumble below 300 Hz.
- **Decision 3 revised:** the engineer wants both mics kept, so the capture
  ring holds both channels and the encoder gets a selectable mono mix
  (left, right, or the average, the default). Both channels stay available
  for noise suppression or beamforming later.

## G2 result (2026-09-29): PASS

The live mic replaces the simulated one: firmware `1f7ee00` + `4cc54d8`
(`CONFIG_SOAK_AUDIO_HAT`, `boards/audio_hat.overlay`, payload mode MIC).
Two units: the HAT on the TFT unit (SEC 1, `mode mic`, `mix avg`, phase
12 ms) talking, the shield unit (MASTER) listening. 1 min, the engineer
talking into the HAT mics.

Build (TFT unit; the shield unit the same with its own two files):

```
west build -b nrf5340dk/nrf5340/cpuapp -d build-hat-tft . -- \
  -DEXTRA_DTC_OVERLAY_FILE="boards/nrf5340dk_nrf5340_cpuapp_tft.overlay;boards/audio_hat.overlay" \
  -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.conf \
  -DCONFIG_SOAK_C2_ENCODE=y -DCONFIG_SOAK_AUDIO_HAT=y
```

| Check | Result |
|---|---|
| Chunks staged by the talker | 743; 0 late (past the pickup deadline), 0 busy |
| Received by the listener | 743 / 743, 0 missing; decoded audio sample-for-sample equal to the talker's own TX records |
| Engine | PER 0 %, `slot_timeouts` 0, CRC errors 0; `stale_retx` 1 (the TX slot before the first chunk, as in every PCM run) |
| Worst frame encode / capture-to-stage | 3.95 ms / 4.00 ms, so ~8 ms of slack at the 12 ms phase |
| Pre-TX pickup margin | min 1.73 ms, as before for a unit with a payload to write (≥ 1.69 ms, pre-TX pickup entry) |
| Mic path | LRCLK 7999.96 Hz; 0 slips in 2975 frames; 0 I2S restarts; 0 clipped samples (mic peaks 22.6k / 27.5k of 32.8k) |
| Talker CPU busy | 26.8 % (encoder 15.6 %) |
| Listening | "pretty robotic, but it does sound like me and it's clear": Codec 2 3200 as expected. `soaks/rtt/g2/g2_heard_by_master_2.wav` |

- **The no-RTT images are byte-identical** to before G2 on both unit types,
  so the TX path there is unchanged.
- `tools/mic_wav.py` decodes a mic stream from a log: the talker's TX
  records, or a peer's RX records with `--slot`.
- **Bench trouble on the way, both hardware:**
  - The HAT DK's SX1262 did not come up (BUSY stuck high, `tdma_init`
    -ENODEV, the pre-HAT image too) until a radio wire was reseated. The
    radio driver probes only at boot, so the unit needed a reset afterwards.
  - From a cold power-up the codec did not answer 300 ms after boot. The
    firmware now retries its setup for ~5 s (`4cc54d8`).
- **Open for later:**
  - The robotic quality is Codec 2 at 3200 bit/s. Things to try: a
    100–150 Hz high-pass ahead of the encoder (the idle floor is
    rumble), and mic gain staging.
  - One run, one talker, one listener. G3 adds a third unit and playout.
