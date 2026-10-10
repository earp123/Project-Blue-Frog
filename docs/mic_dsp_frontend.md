# Mic DSP front end: HPF + noise suppression + AGC ahead of Codec 2

**Status:** task, 2026-10-09. **Follows:**
[`live_audio_wm8960.md`](live_audio_wm8960.md) (G3: Codec 2 3200 is the
quality bottleneck; the bench input was already clean, −19 to −24 dBFS).
**Does not touch:** the engine, the payload size, the codec, or the
airtime decision. Those wait for the post-RF_SW field soak.

**Goal:** condition the 20 ms mono frames the encoder is fed so that
Codec 2 3200 gets the cleanest possible input under field noise (crowd,
wind, handling, PA). Codec 2 is a sinusoidal/LPC vocoder: its pitch
tracker (NLP) and LPC fit degrade sharply with low-frequency rumble and
broadband noise, and its energy quantiser likes a steady level. Nothing
here changes the bytes on air.

**Expectation, stated up front:** this will **not** make 3200 bit/s sound
less robotic on a quiet bench — the A/B already showed the raw input is
fine there. The payoff is in noise, where a vocoder falls apart faster than
a waveform codec. If D1 shows no difference in noise, stop and remove it.

## Where it goes

`c2_enc.c` → `encode_frame()`, mic path only, between
`hat_audio_frame(frame_pcm, FRAME_SAMPLES)` and `codec2_encode()`:

```
hat_audio_frame()  →  [ HPF → NS → AGC ]  →  codec2_encode()
                   ↑ "RM" raw tap           ↑ new "RC" conditioned tap
```

- Runs on the encoder thread (preemptible, same as the encode), so it
  takes CPU from the UI, never the radio.
- Keep the existing `rawmic` tap **before** the DSP and add a second tap
  after it, so the host can A/B raw / conditioned / coded from one run.
- Everything behind one Kconfig, `SOAK_MIC_DSP` (depends on
  `SOAK_AUDIO_HAT`), plus a run-time `dsp on|off` and per-stage
  `dsp hpf|ns|agc on|off` over RTT, default all on. Off must be
  bit-identical to today's path.

## Design decisions (made; revisit only if a gate fails)

1. **Use the SpeexDSP preprocessor first, do not hand-roll.**
   `speex_preprocess_run()` is denoise + AGC + VAD in one call, BSD
   licensed (product-safe, unlike Codec 2's LGPL), frame-based at exactly
   our 8 kHz, and runs on Cortex-M. Vendor only `preprocess.c`,
   `filterbank.c`, `fftwrap.c` (+ `smallft.c` or point it at CMSIS, the
   codec build already pulls `arm_rfft_fast_f32` in under
   `SOAK_C2_ARM_FFT`), `mdf.c` not needed. Float build; the M33 has an
   FPU and the encoder already selects `FPU_SHARING`. Hand-rolled Wiener
   / spectral subtraction on CMSIS-DSP is the fallback **only if D0
   fails**.
2. **HPF is a separate 2nd-order Butterworth biquad at 150 Hz** in front
   of Speex (`arm_biquad_cascade_df1_f32`, or six lines of C). Speex's
   denoiser is not a rumble filter. Male F0 bottoms out around 85 Hz, but
   Codec 2 re-synthesises pitch from the tracker, not from the fundamental
   itself, so 150 Hz costs nothing audible and kills wind and handling
   noise. If pitch tracking of low male voices is audibly worse, drop to
   100 Hz and report.
3. **Latency: one DSP frame.** The Speex preprocessor's overlap-add
   delays its output by one frame. Run it on **80-sample (10 ms)
   sub-frames**, two per 20 ms encoder frame, so the added mouth-to-ear
   latency is 10 ms, not 20. 128-point FFT, 62.5 Hz bins — fine for
   speech NS. Measure and report the actual added delay (G4 will want it).
4. **Order: HPF → NS → AGC.** AGC after NS so it does not pump the noise
   floor up in pauses. Speex does NS before AGC internally; the HPF is
   ours in front.
5. **AGC target −20 dBFS RMS** (where the clean bench input already sat),
   max gain +20 dB, max attenuation −10 dB, slow (Speex defaults
   `AGC_INCREMENT`/`AGC_DECREMENT` are a reasonable start). Hard ceiling:
   no sample may clip; Speex's AGC has a `AGC_MAX_GAIN` and the output is
   saturated anyway. Note for later: once the VPU/VOX gate exists it, not
   the AGC, decides when the mic is live; the AGC must never bring up
   room noise in a gated-off state — leave the VAD output exposed in
   stats so that design can use it.
6. **NS strength:** start at Speex `NOISE_SUPPRESS = −15 dB` (default is
   −15). More than −20 dB produces musical noise that a vocoder then
   amplifies into warble; do not chase it.
7. **No echo cancellation, no dereverb.** Earpieces only (decision 6 of
   the live-audio task), so there is no acoustic return path.

## Gates, in order (Fail Fast)

Stop at the first gate that fails and report; do not polish later gates.

**D0 — CPU and memory, on the device, before any listening.** Time the
full chain (HPF + two Speex sub-frames) per 20 ms frame at 128 MHz, same
flags as the encoder (`-O2`, `-fsingle-precision-constant`).
- Budget: **≤ 1.5 ms per 20 ms frame** (7.5 % of the core; keeps the
  4-unit total under ~72 % with the 64 % measured at G0). Expected
  ~0.3–0.6 ms.
- State: Speex preprocess state + biquad < 8 KB; encoder thread stack
  unchanged or report the increase.
- Fail → report the numbers and stop. Decision 1's fallback is a
  separate decision.

**D1 — Bench A/B, quiet and in noise.** One HAT unit in mic mode,
`rawmic on`, both taps captured. Talker 30 cm from the mic, reading the
same 30 s passage four times:
1. quiet bench, DSP off; 2. quiet bench, DSP on;
3. **noise**: a phone/laptop playing a stadium-crowd recording at
   70–75 dB SPL at the mic (a phone SPL-meter app is good enough), DSP off;
4. same noise, DSP on.

Extend `raw_mic_wav.py` to write raw / conditioned / Codec 2-of-conditioned
as aligned tracks (it already does raw and Codec 2-of-raw from the soak
file). Score with **STOI** (`pip install pystoi`, pure Python, no host
compiler — the PESQ blocker does not apply) using the quiet raw track as
the reference, and listen.
- Pass:
  - **In noise:** Codec 2 of the conditioned input is judged more
    intelligible than Codec 2 of the raw input by the engineer, and STOI
    improves by ≥ 0.05.
  - **In quiet:** no audible pumping, musical noise or dulling; STOI
    within 0.02 of DSP-off. Peak level never clips (`peak[]` in
    `hat_audio_stats`).
- Fail in noise → the front end does not earn its place on this codec;
  remove it and record the STOI numbers in the CHANGELOG (that is still a
  result). Fail in quiet only → tune NS strength / AGC speed once, re-run
  once, then report.

**D2 — Timing and radio untouched.** 1-minute three-unit run, DSP on, at
the 12 ms phase.
- Pass: 0 late stages; staging lead margin unchanged from G2 (≥ 2.45 ms);
  `stale_retx` 0, `slot_timeouts` 0; `hat` slips 0. Report the added
  audio latency from decision 3 as a number.

## Telemetry to add (`hat` status over RTT)

- DSP: per-frame CPU avg/max (µs), current AGC gain (dB), Speex's noise
  floor estimate (dB) and VAD state, HPF on/off, per-stage on/off.
- Keep the TX path instruction-identical with DSP off (Kconfig off → no
  code; run-time off → bypass), as the previous tasks did.

## Deliverables

- Firmware on `TDMA_shim`, both unit types building (`-p always`), DSP
  default **on** for HAT builds only if D1 passes, otherwise the Kconfig
  stays off and the code stays for the field test.
- Vendored SpeexDSP files under `src/third_party/speexdsp/` with its
  `COPYING` (BSD) alongside; note the licence in the README's
  third-party section.
- `raw_mic_wav.py` writing the conditioned track; the four D1 WAVs kept
  under `soaks/` next to the logs.
- CHANGELOG entry in the house style: D0/D1/D2 numbers, what passed, what
  is open; update "Where things stand" and the "Next" list.
- Update the G3 section of `live_audio_wm8960.md` with a one-line pointer
  here.

## Open questions (do not block)

- Speex's AGC is tuned for VoIP conversational levels; a referee shouting
  at 10 cm may need a shorter attack. Collect the peak stats in D1 before
  touching it.
- Whether the HPF belongs in the WM8960 instead. Its ADC high-pass
  (R5 `ADCHPD`) is a DC-blocking filter by default; check the datasheet
  for whether its cut-off can be raised anywhere near 150 Hz. Even if it
  can, a software filter is testable and the same on any future codec
  part. Software first.
