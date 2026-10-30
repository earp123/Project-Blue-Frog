# Opus at 7.2 kbit/s on an 84-byte packet: the airtime lever

**Status:** task, 2026-10-30. **C0 passed (2026-10-30); C2 brought
forward ahead of C1 and running on four units** (the engineer asked to hear
Opus over the radios first, which needs the 84 B engine, not the device
codec), results at the end. **Follows:**
[`live_audio_wm8960.md`](live_audio_wm8960.md) (G3: Codec 2 3200 is the
quality limit), [`mic_dsp_frontend.md`](mic_dsp_frontend.md), and the
range work through 2026-10-30 (CHANGELOG). **Decision this rests on:**
with the Molex 2067640100 the link runs −56 dBm LOS at 134 m, so 200 m
has ~27–35 dB of margin at SF5. **No more sensitive modulation is
needed**, and the slot's spare airtime goes to the payload instead.

**Goal:** replace Codec 2 3200 with Opus (SILK, 8 kHz, CBR) on a bigger
packet, measure what it buys, and settle the codec licence at the same
time (Opus is BSD; Codec 2 is LGPL, "Next" item 4).

**Decisions already taken with the engineer (2026-10-30):** Opus SILK at
8 kHz; L3 payload 40 → **80 bytes** (84 on air); **4 × 20 ms frames** per
80 ms chunk, so the encode pipeline keeps its shape.

## The numbers

Slot 20 ms, active window 18 ms, boundary → air 532 µs (measured).
`t_sym` 64 µs, preamble 1168 µs. Payload symbols
`8 + 5 * ceil((8*LEN + 24) / 20)` (tdma.h's formula, LEN = bytes on air).

| on air | symbols | TOA | TX done at | slot left after RX done |
|---|---|---|---|---|
| 44 B (today) | 103 | 7 760 µs | 8.3 ms | 9.7 ms |
| 68 B | 153 | 10 960 µs | 11.5 ms | 6.5 ms |
| **84 B** | **183** | **12 880 µs** | **13.4 ms** | **4.6 ms** |

Payload budget per 80 ms chunk at 80 B: 8 B test header (keep, see
decision 6) + 4 × **18 B Opus packets** (1 B TOC + 17 B SILK) =
**7.2 kbit/s** set, 6.8 kbit/s of speech data. Published MOS for
Opus NB at 6–8 kbit/s sits ~0.5 above Codec 2 3200; this task measures
it on our clips rather than trusting that.

## Design decisions (made; revisit only if a gate fails)

1. **libopus 1.5.x, vendored unmodified under `external/opus/`**
   (BSD, next to speexdsp). Build flags: `FIXED_POINT`,
   `DISABLE_FLOAT_API`, `OPUS_BUILD`, `VAR_ARRAYS` off and
   `NONTHREADSAFE_PSEUDOSTACK` on with a static scratch arena (as speexdsp
   got), `-O2`. CELT cannot be left out of libopus but is never called
   at 8 kHz SILK; accept the flash (~200 KB of 1 MB).
2. **Encoder:** `OPUS_APPLICATION_VOIP`, `OPUS_SET_BANDWIDTH(NARROWBAND)`,
   `OPUS_SET_SIGNAL(VOICE)`, `OPUS_SET_VBR(0)` (CBR, so `opus_encode()`
   returns exactly 18 B at `OPUS_SET_BITRATE(7200)`), `OPUS_SET_DTX(0)`,
   `OPUS_SET_INBAND_FEC(0)`, complexity start at **2**, and
   `OPUS_SET_FORCE_MODE(MODE_SILK_ONLY)` (request 11002, `opus_private.h`):
   left to choose, libopus codes any 20 ms frame under 15 B as CELT (the
   "less than 6 kb/s" rule in `opus_encode_native()`'s auto branch). At
   7.2 kbit/s forcing changes no byte (C0); it keeps a smaller frame,
   decision 9's fallback, in SILK. FEC stays off: the
   four frames of a chunk are lost together, so LBRR of frame k−1 inside
   frame k protects nothing we lose. One encoder state per run, as Codec 2
   has.
3. **Decoder + PLC:** one `OpusDecoder` per peer, persistent per run. A
   chunk-index gap of `d` chunks runs `opus_decode(dec, NULL, 0, pcm,
   160, 0)` four times per missing chunk, **up to 2 chunks (160 ms)**, and
   mixes that into playout where today's `hat_audio_play_chunk()` path
   leaves silence; beyond 2 it is silence, and the decoder still gets the
   PLC calls so its state follows real time. This replaces the "lost chunk
   = 80 ms hole" of G3 for free.
4. **Engine: payload length becomes a Kconfig, TOA becomes a formula.**
   `TDMA_PAYLOAD_LEN` from `CONFIG_TDMA_PAYLOAD_LEN` (40 or 80; both units
   in a group must match, the L2 length check already rejects a mismatch),
   and `TDMA_TOA_US` computed from `TDMA_ON_AIR_LEN` by the macro above
   (integer form: `1168 + 64 * (8 + 5 * ((8*LEN + 24 + 19) / 20))`), with a
   `BUILD_ASSERT` that it gives 7760 at 44 B. No hand-typed TOA anywhere —
   learnings: every TOA/latency bug so far was a constant drifting from
   its definition. `master_slot0 = rx_ts − TOA − 532` then follows on its
   own. TX buffer 0x00–0x53 and RX 0x80–0xD3 still do not overlap.
5. **Pre-TX pickup 2500 → 3500 µs.** The pickup must cover wake plus one
   WriteBuffer; 84 B at the current SPI rate is ~1.4 ms against ~0.7 ms
   today. 3.5 ms keeps the same spare. `pre_tx_margin_min_us` is the check.
   (Raising the SPI clock is a separate lever; not here.)
6. **Keep the 8 B test header** (magic 0xC2, codec, chunk index): it is
   what loss accounting, PLC and the host A/B hang off. New codec id
   `CLIP_CODEC_OPUS = 1`. `CLIP_CHUNK` becomes `TDMA_PAYLOAD_LEN − 8` = 72.
7. **Soak log record 64 → 128 B (format v3) when `TDMA_PAYLOAD_LEN` is 80**,
   four per sector; the META record says which, and `decode_soak_log.py`
   takes `REC_SIZE` from META, not a constant. Alternative rejected:
   logging only the first 40 B of the payload would make a peer's audio
   unreconstructable from the log, and the host A/B needs it. SD rate
   roughly doubles (~6.5 KB/s); RTT capture must keep up — C2 checks.
8. **Codec is a Kconfig choice, `SOAK_CODEC`: `C2_3200` | `OPUS_NB`.**
   Codec 2 stays selectable for the A/B and for the 40 B engine
   regression; it leaves the product build, which is what closes the
   LGPL item. The mic DSP front end sits in front of either.
9. **Nothing else moves.** Slot width, slot count, SF/BW/CR, preamble,
   the sync constants, the HAT path, playout ring: unchanged. If C2 shows
   84 B does not fit, the fallback is 68 B (4 × 14 B, 5.6 kbit/s), not a
   slot change. C0 says it is a weak one: SILK-forced 5.6 kbit/s scores
   the STOI of Codec 2 3200 on clean speech (0.794 vs 0.791; ESTOI +0.05),
   so it buys loss concealment, not intelligibility.

## Gates, in order (Fail Fast)

Stop at the first gate that fails and report; do not polish later gates.

**C0 — Is it worth it? Host only, before any firmware.** Encode the
existing clips (`clip_src`, the D1 raw-mic captures, `mic_dsp_ab.py`'s
material) with libopus at the exact settings of decision 2 and with
Codec 2 3200; decode both; score STOI against the clean source (the
PESQ blocker stays; `brew install opus` / `apt libopus0` + `pip
install opuslib` is a binary, no compiler). Repeat with 80 ms burst
losses at 2 %, 5 % and 10 % (PLC on for Opus, silence for Codec 2 as
today). Listen.
- Pass: Opus 7.2 kbit/s CBR ≥ +0.08 STOI over Codec 2 on clean speech
  and the engineer hears a clear step up. If it is not clearly better at
  7.2 kbit/s on a host, no firmware will make it so: stop, and the answer
  to "how bad is 3200" is this table.
- Also report 5.6 kbit/s (14 B frames) so the 68 B fallback has a number.

**C1 — CPU and memory on the device.** Opus fixed-point encode and decode
of 20 ms NB frames at 128 MHz, `-O2`, complexity 0/2/4, timed like
the Codec 2 profile (`c2_prof`).
- Budget: **encode ≤ 5 ms per 20 ms frame, decode ≤ 1.5 ms per frame per
  peer**, i.e. the 4-unit case (1 enc + 3 dec) ≤ 70 % of the core, G0's
  bar. RAM: encoder + 3 decoders + scratch ≤ 128 KB; report the encoder
  thread stack high-water mark against today's 128 KB.
- Fail → drop complexity to 0; still over → report and stop (decision 9
  does not help CPU; the next lever would be 2 × 40 ms frames, a
  separate decision).

**C2 — Engine at 84 B, no audio.** Two units, ramp payload, 30-minute
bench soak at +0 dBm, both built with `CONFIG_TDMA_PAYLOAD_LEN=80`.
- Pass: `tx_evt_dt_us` reads **13 412 ± 30 µs** (TOA + 532) on both
  roles — the standing regression check, now at the new length;
  phase_err σ and worst within the 2026-08-18 envelope (σ ≈ 17 µs, worst
  ±75 µs); `pre_tx_margin_min_us` ≥ what it is today at 40 B; PER 0,
  `stale_retx` 0, `slot_timeouts` 0; v3 log decodes end to end and the
  RTT capture drops 0 records. Then the same build at 40 B once, to show
  the Kconfig path did not move the old numbers.

**C3 — Live, three units, A/B by ear.** HAT units, mic mode, Opus build,
separate rooms, 5 minutes with every unit talked into in turn — this is
also G3's formal run, on the new codec. Then flash the Codec 2 build and
repeat 1 minute for the A/B.
- Pass: G3's own marks (0 underruns after fill, corrections consistent
  with crystal offset, radio counters clean), PLC audibly bridging single
  lost chunks instead of a hole (pull one unit's antenna for a second to
  force some), and the engineer's verdict that Opus is the one to ship.
- Then G4 latency as written, on this build: expected ≈ 100 ms + ~5 ms
  (the later RX done) + decode + playout.

## Telemetry to add

- Per peer: PLC chunks synthesised, chunks silenced past the PLC cap.
- Encoder/decoder µs per frame avg/max, complexity in use, Opus state
  bytes, scratch high-water mark.
- META: codec id, payload length, record size.

## Deliverables

- Firmware on `TDMA_shim`, both unit types building (`-p always`) in
  both codec choices and both payload lengths.
- `external/opus/` with `COPYING`; README third-party section updated;
  the CHANGELOG "Next" item on Codec 2's LGPL closed or restated.
- Host: `reconstruct_opus.py` (peer audio from a v3 log), the C0 script
  and its table checked in under `tools/`, the D1-style WAVs under
  `soaks/`.
- CHANGELOG entry in the house style: C0 table, C1 numbers, C2 regression
  figures, C3 verdict; update "Where things stand" and "Next".
- One line in `live_audio_wm8960.md` G3 and in `tdma_layering.md` where
  TOA and the pickup are described.

## Open questions (do not block)

- Whether the 8 B test header shrinks to 4 B in the product (→ 19 B
  frames, 7.6 kbit/s). Not now; it is the loss-accounting hook.
- SPI clock to the radio: 84 B WriteBuffer/ReadBuffer at ~1.4 ms each is
  the single biggest fixed cost left in the slot. Worth a look after C2.
- Whether complexity above 2 is audible at 7.2 kbit/s NB. C1 gives the
  CPU cost; C3 can try it if the budget allows.

## Results

Details and figures: the CHANGELOG entry of 2026-10-30.

- **C0, passed (2026-10-30).** On clean speech STOI went 0.791 → 0.878
  (+0.086 against the +0.08 bar) and ESTOI 0.636 → 0.794. The engineer
  heard a clear step up ("wayyyy better"). The table is in
  `soaks/opus_c0/table.txt`, the A/B WAVs beside it.
  - The host libopus was 1.3.x (PyOgg). The device's 1.5.2 fixed-point
    build has decoded the same clips bit-exact over the air.
  - The 68 B fallback is weak: forced to SILK at 5.6 kbit/s it scores
    Codec 2's STOI (decision 9).
- **C2, run ahead of C1, with one check open.** The 84 B engine runs on
  two and four units. Every chunk received was bit-exact, after card #23
  (the receive-buffer wrap) was fixed in `tdma_radio.c`.
  - Phase error, pickup margin, stale re-sends and slot timeouts are all
    inside C2's marks.
  - **Open:** `tx_evt_dt_us` reads 13,356 µs against 13,412 ± 30. The
    slot clock checks out, so the airtime at 84 B is ~56 µs shorter than
    the formula, which is exact at 44 B.
  - The runs were 5 and 9.5 minutes, not 30, and on clips, not the ramp.
- **C1, decode half, passed.** About 0.5 ms per 20 ms frame against the
  1.5 ms bar; 6.6–9.5 % of the core for three peers; 17.8 KB per decoder.
  The encode half is not done.
- **C3, informally.** Three HAT units play their peers' Opus clips
  (`CONFIG_SOAK_OPUS_DEC`). PLC must conceal on time to keep the playout
  steady (opus_dec.h). Two units sound great. SEC 1 (1050080691) sounds
  bad with clean counters, so its HAT is suspect. G3's formal run needs
  the device encoder (mic mode).

### Running it

```sh
# Clips (host): four 18 B frames per 72 B chunk, plus <clip>_op72_ref.wav
python src/tdma_console/tools/c2clip.py opus soaks/clips/OSR_us_000_0010_8k.wav -o soaks/clips/OSR_0010_F.op72

# A HAT unit that decodes and plays (shield shown; the TFT the same with its files)
west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-opdec-m --   "-DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.overlay;boards/audio_hat.overlay"   -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.conf   -DCONFIG_TDMA_PAYLOAD_LEN=80 -DCONFIG_SOAK_OPUS_DEC=y -DCONFIG_SOAK_AUDIO_HAT=y   -DCONFIG_SOAK_UNIT_SLOT=0 -DCONFIG_SOAK_BOOT_TX_POWER_DBM=-9

# Per unit, after every boot (the clip lives in RAM): upload, clip mode
python src/tdma_console/tools/rtt_link.py --sn <serial> clip soaks/clips/OSR_0010_F.op72
python src/tdma_console/tools/rtt_link.py --sn <serial> mode clip

# All units: soak, capture, live opus/play counters; --play SLOT also plays
# that unit's ears on the PC (keys 0-3 in a real console)
python src/tdma_console/tools/rtt_link.py --all capture -o soaks/<run>/ --soak 5
python src/tdma_console/tools/loss_conditions.py soaks/<run>/*.bin
```
