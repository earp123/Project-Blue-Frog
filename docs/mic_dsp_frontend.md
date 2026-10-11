# Mic DSP front end: HPF + noise suppression + AGC ahead of Codec 2

**Status:** built and measured, 2026-10-10. **D1 is split:** in near-stationary crowd roar Codec 2's intelligibility rises by +0.078 STOI, above the +0.05 bar. In babble from nearby talkers and in wind the gain is nil, and a soft talker in quiet loses 0.027. D0 is over budget, at 1.7–1.8 ms per frame against 1.5. D2 passes on two units (the third was not on USB). So `CONFIG_SOAK_MIC_DSP` stays **off by default**, and the code stays in for a field A/B by ear. Results are below.

**Follows:** [`live_audio_wm8960.md`](live_audio_wm8960.md). G3 found that Codec 2 3200 is the quality bottleneck, and the bench input was already clean, at −19 to −24 dBFS.
**Does not touch:** the engine, the payload size, the codec, or the airtime decision. Those wait for the post-RF_SW field soak.

**Goal:** condition the 20 ms mono frames the encoder is fed, so that Codec 2 3200 gets the cleanest possible input under field noise (crowd, wind, handling, PA). Codec 2 is a sinusoidal/LPC vocoder. Its pitch tracker (NLP) and its LPC fit both degrade sharply with low-frequency rumble and broadband noise, and its energy quantiser likes a steady level. Nothing here changes the bytes on air.

**Expectation, stated up front:** this will **not** make 3200 bit/s sound less robotic on a quiet bench, because the A/B already showed the raw input is fine there. The payoff is in noise, where a vocoder falls apart faster than a waveform codec. If D1 shows no difference in noise, stop and remove it.

## Where it goes

`c2_enc.c` → `encode_frame()`, between taking the frame and `codec2_encode()`. That covers both modes that encode: **mic** (live HAT frames) and **pcm** (the uploaded clip). pcm mode is what makes D1 repeatable and sample-aligned.

```
hat_audio_frame() or clip  →  [ HPF → NS → AGC ]  →  codec2_encode()
                           ↑ "RM" tap              ↑ "RC" tap
```

- [`src/tdma_console/mic_dsp.{c,h}`](../src/tdma_console/mic_dsp.h) runs on the encoder thread, which is preemptible like the encode. It takes CPU from the UI, never from the radio.
- With `rawmic on`, up channel 3 carries the frame as captured ("RM"). When the DSP ran, it also carries the frame after it ("RC"), which is what Codec 2 encodes. `raw_mic_wav.py` writes `_raw.wav` and `_cond.wav`.
- Everything sits behind `CONFIG_SOAK_MIC_DSP` (depends on `SOAK_C2_ENCODE`). Once built in it starts on, with all stages enabled. Over RTT: `dsp on|off`, `dsp hpf|ns|agc on|off`, and for tuning from the next soak, `dsp level <n>` and `dsp nsdb <dB>`. Off means the frame is not touched at all. With the Kconfig off, no code is linked, and the default build is unchanged except for the encoder's frame-source refactor. In pcm mode, `rawmic on` now streams the "RM" tap too.

## Design decisions (as built)

1. **SpeexDSP preprocessor, not hand-rolled.** BSD licensed, frame-based, Cortex-M friendly. Vendored in [`external/speexdsp`](../external/speexdsp/README.md) (tag `SpeexDSP-1.2.1`, sources unmodified): `preprocess.c`, `filterbank.c`, `fftwrap.c`, `smallft.c` and their headers. Three corrections to the original plan:
   - **`mdf.c` is needed after all, or a stub.** `preprocess.c` calls `speex_echo_get_residual()` whenever an echo state is attached. None ever is, so `port/echo_stub.c` resolves the symbol.
   - **No CMSIS FFT.** The preprocessor's FFT is 2 × the frame size, 160 points for 80-sample frames. That is not a power of two, so `arm_rfft_fast_f32` cannot take it. Speex's `smallft` does it. KISS FFT (Speex's other option, with a dedicated radix-5 butterfly) measured no faster: the two FFTs are ~23 % of the preprocessor's time either way.
   - **Float maths must be forced to single precision.** The float build calls `exp`, `log`, `pow`, `sqrt` and the rest in double, which the M33 does in software. That cost 10.7 ms per 20 ms frame. `port/float_math.h`, force-included, maps them to the float functions: 1.9 ms.

   The state is allocated from a static 13 KB arena in `mic_dsp.c`, with no heap; the state itself is 12,840 B. It is rebuilt at each run's start.
2. **HPF:** a separate 2nd-order Butterworth biquad at 150 Hz in front of Speex (RBJ coefficients, transposed direct form II, float). It costs ~70 µs per frame including the stats.
3. **Latency:** Speex runs on two 80-sample (10 ms) sub-frames per 20 ms frame, so its overlap-add adds one sub-frame. Measured end to end on the RC tap: 96 samples (12 ms ± 2 at the scorer's 4 ms resolution). The FFT is 160 points with 50 Hz bins, not 128/62.5 Hz as first written.
4. **Order:** HPF → NS → AGC. Speex does NS before AGC internally.
5. **AGC:** Speex's controls are a target level (in its own loudness units; the default, 8000, landed every condition at −20 to −22.5 dBFS RMS of speech, which is the −20 dBFS goal), a max gain (set to +20 dB), and increment/decrement rates (defaults, +12 / −40 dB/s). **It has no max-attenuation setting**, so the "−10 dB" in the plan does not exist. Its own limiter scales a frame down if any sample would pass 28000.
6. **NS strength:** −15 dB (Speex's default). More produces musical noise that the vocoder turns into warble.
7. **No echo cancellation, no dereverb.** The units use earpieces only, so there is no acoustic path from the output back to the mics.

## Sequence and gates (revised)

The original order was D0 → D1 → D2, stopping at the first failure. It is now **D0 measured → D1 → D2, with CPU work deferred until D1 says the stage is worth it**. D0 is cheap to measure, but making it pass means patching vendored code (see "CPU levers"), and that is wasted if D1 shows the stage does not help.

**D0: CPU and memory.** Per 20 ms frame at 128 MHz, the full chain with its telemetry, on the encoder thread (wall clock, so preemption is included).
- Budget: **≤ 1.5 ms** (7.5 % of the core). Expected 0.3–0.6 ms.
- State: preprocessor + biquad. The < 8 KB target was a guess; the state is 12.5 KB. The encoder thread's stack did not change: 10,144 B never touched, same as before.

**D1: does it help, in pcm mode.** [`tools/mic_dsp_ab.py`](../src/tdma_console/tools/mic_dsp_ab.py): `make` builds 8 s clips from the Open Speech Repository recordings already in `soaks/clips`, and `run` encodes each one on a unit with the DSP off and then on.

- Every run feeds the encoder the same samples, so off and on differ only by the DSP.
- The clean speech under the mix is a sample-aligned STOI reference. This replaces the live talker, whose separate takes could not be scored against each other.
- `score` decodes the unit's own TX records with the reference Codec 2, places chunk n at clip position n mod 100, finds each run's delay by envelope correlation against the input, and scores loops 2–6 (16 s in, settled) with STOI and ESTOI.

Conditions:

| Clip | What it is |
|---|---|
| quiet_M, quiet_F | Male / female talker, no noise, speech at −22 dBFS |
| soft_M | No noise, −34 dBFS (for the AGC) |
| crowd5_M, crowd0_M, crowd0_F | Babble (10 talker streams + pink noise at −8 dB) at +5 / 0 dB SNR |
| roar0_M | Near-stationary roar (40 streams + pink at 0 dB) at 0 dB SNR: a stadium heard from inside it |
| wind0_M | Brown noise low-passed at ~80 Hz, gusting, at 0 dB SNR |

- Pass in noise: Codec 2 of the conditioned input beats Codec 2 of the raw input by **≥ 0.05 STOI**, and by ear.
- Pass in quiet: within 0.02 STOI, with no pumping, musical noise or dulling, and no output clipping (`dsp ... clip=` stays 0).
- STOI ignores level, so the AGC's benefit (a steady output level) shows up only in the level column, not in the scores.
- A live acoustic check by ear in real crowd noise is still the final word. D1 does not replace it.

**D2: timing and radio untouched.** A 1-minute run in mic mode, DSP on, at the 12 ms phase, with every unit in the kit.

- Pass:
  - 0 late stages.
  - Pre-TX margin minimum ≥ 1.69 ms. That is the known floor for a unit with a payload to write; G2 measured 1.73 ms. The original "≥ 2.45 ms" was the pickup deadline itself, not a margin.
  - `stale_retx` ≤ 1. Every mic/pcm run has 1: the TX slot before the first chunk.
  - `slot_timeouts` 0, HAT slips 0, PER 0.

## Results (2026-10-10)

### D0: over budget

| | per 20 ms frame |
|---|---|
| Full chain, mic or pcm mode (avg / max) | **1.67–1.82 ms / 1.71–2.00 ms** (2.26 ms max on a secondary that also decodes a peer) |
| HPF alone | 0.07 ms |
| Speex alone, NS and AGC either way | 1.6 ms (its analysis runs regardless) |
| State | 12,840 B; stack unchanged |

The preprocessor costs ~109k cycles per 10 ms sub-frame (DWT profile, per call):

| Part | Cycles |
|---|---|
| FFT + IFFT | 25k |
| Filterbanks | 12k |
| `expf` / `logf` / `powf` | < 7k |
| The rest | Spread evenly over its per-bin float loops |

`-ffast-math` on the library did not help. Encoder CPU goes from ~17 % to ~25 % of the core. At four units that puts the total around 73 %, against the ~72 % the budget aimed for.

### D1: helps in stationary noise only

STOI against the clean speech, loops 2–6 (ESTOI in [`scores.json`](../soaks/mic_dsp_d1/scores.json)). "c2 off" is what is sent today.

| Clip | Input | After DSP (uncoded) | **c2 off** | **c2 on** | **Δ** |
|---|---|---|---|---|---|
| quiet_M | 1.000 | 0.980 | 0.838 | 0.827 | −0.011 |
| quiet_F | 1.000 | 0.952 | 0.775 | 0.782 | +0.007 |
| soft_M (−34 dBFS) | 1.000 | 0.978 | 0.840 | 0.813 | **−0.027** |
| crowd5_M | 0.753 | 0.748 | 0.658 | 0.674 | +0.016 |
| crowd0_M | 0.650 | 0.642 | 0.580 | 0.596 | +0.016 |
| crowd0_F | 0.394 | 0.395 | 0.367 | 0.362 | −0.004 |
| roar0_M | 0.576 | 0.602 | 0.491 | 0.569 | **+0.078** |
| wind0_M | 0.970 | 0.952 | 0.828 | 0.813 | −0.015 |

- **Roar passes, babble fails.** Speex's noise estimate tracks a steady roar and removes it: Codec 2 goes from 0.491 to 0.569, and the uncoded audio improves too. It cannot track nearby talkers, so in babble the DSP leaves intelligibility where it was, before Codec 2 and after it. That is the usual result for single-channel noise suppression.
- **NS alone** (HPF and AGC off; the `--stages n --label ns` variant):

  | Clip | Δ vs c2 off |
  |---|---|
  | roar0_M | +0.070 |
  | crowd0_M | +0.009 |
  | wind0_M | −0.019 |
  | soft_M | −0.047 |

  So NS earns the roar gain and also costs the soft talker. The full chain does better on soft_M (−0.027) because the AGC brings it up first.
- **The AGC levels speech as intended.** Speech after the DSP sits at −20.1 to −22.5 dBFS RMS in every condition, including soft_M (input −34). Roar is lower, at −26.0, because NS removes energy. No run clipped (`clip=0` everywhere). The highest output peak was 31,601, on quiet_M.
- **Wind** as synthesised here (mostly below 150 Hz) barely hurts Codec 2 in the first place (0.828 vs 0.838 quiet), so the HPF has nothing to win back. Real wind on a mic capsule is broader and can overload the input. That needs the field.
- **Listening:** `soaks/mic_dsp_d1/<clip>_ab.wav` holds one steady loop per clip, left = Codec 2 today, right = Codec 2 after the DSP. The by-ear half of the gate is still open.

### D2: passes on two units

Two units: OLED unit 1050080691 as master, TFT unit 960170130 as SEC 1. Mic mode, DSP on, 1 minute, 12 ms phase. Logs are in `soaks/mic_dsp_d2/`.

| | Master | SEC 1 |
|---|---|---|
| Late stages | 0 | 0 |
| Pre-TX margin min | — (no payload pickups) | **1.694 ms** (≥ 1.69) |
| PER / `stale_retx` / slot timeouts | 0 % / 1 / 0 | 0 % / 1 / 0 |
| HAT slips | 0 | 0 |
| Played (chunks from the peer) | 747 | 752 |
| DSP avg / max | 1.70 / 1.81 ms | 1.93 / 2.26 ms |

- **The margin is 36 µs below G2's 1.73 ms.** It is still above the floor.
- **No three-unit run.** Unit 960166366 did not show up on USB.
- **AGC in an idle room:** the AGC sat at its +18 to +20 dB maximum, and a transient reached 32,427 with no clipping counted. This is the risk noted under decision 5: with no talker, it brings the room up. Any VOX/VAD gate must decide when the mic is live, not the AGC.

### Decision

Per the gate rule ("default on only if D1 passes"), `CONFIG_SOAK_MIC_DSP` stays **off**, and the code stays for the field.

The result is not a plain fail. For a referee in a stadium, the crowd is mostly roar, which is the case the DSP helps; the bench and coaches nearby are babble, which it does not. The deciding test is a field A/B by ear in real crowd noise. Build one HAT unit with `-DCONFIG_SOAK_MIC_DSP=y` (see below) next to units without it.

If it is kept, these come first:
- The CPU levers below, to get under 1.5 ms.
- Either a gentler NS for soft talkers (`dsp nsdb 10`), or NS only (`dsp agc off`) if the AGC behaviour in idle rooms is unwanted.

## CPU levers (if the field says keep it)

Ordered by expected payoff and risk:

1. **Bark-resolution gains.** Speex has a built-in switch, the `if (1)` in `preprocess.c`, which would become a vendored patch like Codec 2's. It drops the per-bin Ephraim-Malah loop, ~20k of the ~109k cycles per call, for 24-band gains. At 8 kHz that should still be fine for speech.
2. **One 160-sample Speex frame per 20 ms** instead of two 80-sample ones. This halves the per-call fixed costs (band loops, gain floor, AGC), adds 10 ms of latency, and needs no source change.
3. **Run the library from RAM** (`zephyr_code_relocate`) to avoid flash wait states. The TFT HAT build has ~10 KB of RAM left with the DSP in, which is tight.

## Telemetry

The status reply, last line:

```
ok dsp on hna frames=3000 us=1707/1868 state=12840/13312 gain=1 noise=31 speech=92 clip=0 peak=28753
```

| Field | Meaning |
|---|---|
| `on` | On/off |
| `hna` | Stages applied (hpf/ns/agc) |
| `frames` | Frames conditioned this run |
| `us` | Per-frame CPU, avg/max |
| `state` | Arena used/size |
| `gain` | AGC gain, dB |
| `noise` | Mean noise estimate, dB |
| `speech` | Speech probability of the last frame, % |
| `clip` | Output samples at full scale this run |
| `peak` | Peak since the last status |

## Running it

```sh
# A DSP build (the HAT builds plus one option):
west build -b nrf5340dk/nrf5340/cpuapp -d build-hat-shield-dsp . -- \
  -DEXTRA_DTC_OVERLAY_FILE="boards/nrf5340dk_nrf5340_cpuapp_shield.overlay;boards/audio_hat.overlay" \
  -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.conf \
  -DCONFIG_SOAK_C2_ENCODE=y -DCONFIG_SOAK_AUDIO_HAT=y -DCONFIG_SOAK_MIC_DSP=y

# D1 (role picked on the unit; it runs alone):
python src/tdma_console/tools/mic_dsp_ab.py make
python src/tdma_console/tools/mic_dsp_ab.py run --sn <J-Link SN> [clip ...]
python src/tdma_console/tools/mic_dsp_ab.py score

# Live, any HAT DSP build:
python src/tdma_console/tools/rtt_link.py --sn <SN> dsp off
python src/tdma_console/tools/rtt_link.py --sn <SN> rawmic on   # then capture; raw_mic_wav.py
```

## Open questions (do not block)

- Speex's AGC is tuned for VoIP conversational levels. A referee shouting at 10 cm may need a shorter attack.
- Whether the HPF belongs in the WM8960 instead. Its ADC high-pass (R5 `ADCHPD`) is a DC-blocking filter; check whether its cut-off can be raised near 150 Hz. Software first either way.
