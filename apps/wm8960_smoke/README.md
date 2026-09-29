# WM8960 HAT smoke test (temporary, branch `wm8960_smoke`)

One nRF5340 DK, one BFab/Waveshare WM8960 Audio HAT, no radio or display.
It checks the wiring and the codec setup, and gives a first listen through
the HAT's speaker terminals and headphone jack. The pin map is the one in
[`docs/live_audio_wm8960.md`](../../docs/live_audio_wm8960.md).

## Build and flash

```
west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-wm8960 apps/wm8960_smoke
west flash -d build-wm8960 --dev-id <J-Link serial>
```

The build links four speech clips from `soaks/clips/` (gitignored, like
the other clip files):

- `OSR_0010_F.pcm` and `OSR_0030_M.pcm`: the first 8 s (64000 samples) of
  `OSR_us_000_0010_8k.wav` and `OSR_us_000_0030_8k.wav`, as raw 16-bit
  little-endian mono.
- `OSR_0010_F_bp.pcm` and `OSR_0030_M_bp.pcm`: the same clips band-limited
  to 300–3400 Hz and peak-normalised to −1 dBFS, made with
  `python tools/speech_filter.py soaks/clips/OSR_0010_F.pcm soaks/clips/OSR_0010_F_bp.pcm`
  (and the same for `0030_M`).

## Use

At boot, after a good codec init, the unit plays a 1 kHz tone for 3 s.

| DK button | Mode |
|---|---|
| 1 | Tone: 1 kHz at −12 dBFS. |
| 2 | Speech, looping. Presses cycle: F filtered, M filtered, F raw, M raw. |
| 3 | Mic to headphones only; presses cycle left mic, right mic, each mic to its own ear. |
| 4 | Stop. |

The shell runs on the DK's VCOM at 115200 (`wm help`).

- The modes: `wm tone [Hz] [dBFS] [dc]`, `wm play f|m|fr|mr`,
  `wm loop l|r|lr` and `wm stop`.
- Levels: `wm spk <code>` sets the speaker volume, `wm vol <code>` the
  headphone volume (0x79 = 0 dB, 1 dB per step). `wm gain <pga> <boost>`
  sets the mic gain.
- `wm stat` shows the measured sample rate, both mics' levels and the I2C
  retry count.
- Debug: `wm rec` records 8 s from both mics into RAM, and
  `tools/wm_smoke.py rec` pulls that over J-Link as WAV files.
  `wm reg`, `wm shift` and `wm fix` are bring-up knobs.

## What bring-up found (2026-09-29)

- **The HAT has no I2C pull-ups**, because a Pi provides its own. Without
  pull-ups the bus idles low and nothing ACKs. The overlay turns on the
  nRF's internal pull-ups, which work, but some writes still fail and
  `wm8960_write()` retries them. Fit 2.2–4.7 kΩ pull-ups to 3.3 V on
  SDA and SCL for real units.
- **The clocks are good.** The codec is the I2S master, with its PLL
  running from the 24 MHz crystal (SYSCLK 12.288 MHz). Measured LRCLK is
  7999.96–7999.97 Hz (−5 ppm) against the DK's 32 kHz crystal.
- **I2S words are 24-bit.** The codec's BCLK is SYSCLK / 32, which gives
  48 clocks per frame, exactly two 24-bit words.
- **The codec reads each DAC word one bit clock early.** Its MSB is the
  last bit of the previous slot. Unfixed, every zero crossing becomes a
  full-scale step: speech plays as a loud square wave of its sign
  (recognisable but garbled), while a tone sounds nearly normal.
  - `pack_tx()` in `src/main.c` fixes it. Each sample is shifted up one
    bit, and bit 0 of each word carries the next sample's sign, at a cost
    of one frame (125 µs) of delay.
  - Measured through the speaker into the HAT's mic: before the fix, the
    output did not change with the digital level and THD was about
    −10 dB. After it, the output follows the level and THD is −25 to
    −39 dB.
  - The ADC direction is aligned and needs no fix.
- **Speech on the speaker:** the band-limited clips sound clearly better
  than the raw ones. About half the raw clips' energy is below 300 Hz,
  which a small speaker cannot reproduce.
- **The mics** (LINPUT1/RINPUT1, Waveshare's gains of +12 dB PGA and
  +29 dB boost) work but have not been evaluated yet. The idle floor is
  dominated by rumble below 300 Hz.
