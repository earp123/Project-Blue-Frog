# On-device Codec 2 encode test

**Status:** done, 2026-09-26. **Follows:**
[`rtt_link_c2_transport.md`](rtt_link_c2_transport.md), which proved the
link carries Codec 2 frames intact, and
[`stage_lead_sweep.md`](stage_lead_sweep.md), which set the staging deadline
at 2.45 ms before a unit's TX boundary. This test encodes on the nRF5340
itself and asks: how long does encoding take, is the output right, and how
much latency is left from a sample being captured to it reaching a peer?

## Setup

- **Codec 2 on the device.** The codec is vendored into `external/codec2/`
  (LGPL-2.1; release 1.2.0, commit `06d4c11`) by `scripts/vendor_codec2.py`.
  The script copies the codec's own sources plus their include closure,
  generates `version.h` and the codebook tables (a Python port of upstream's
  host-side `generate_codebook.c`; this machine has no host C compiler),
  and adds the licence. It is linked only with `CONFIG_SOAK_C2_ENCODE`
  (default off), and only mode 3200 is compiled in. The product licensing
  of Codec 2 is not settled; this is test firmware.
- **A frame-locked simulated microphone** (`src/tdma_console/c2_enc.c`).
  - The clip buffer (128 KB in encode builds) holds 8 s of raw 8 kHz
    16-bit PCM, uploaded over RTT (`c2clip.py pcm`, then
    `rtt_link.py clip x.pcm`).
  - The 80 ms chunk for a unit's TX boundary *B* becomes available at
    *B − phase* (`rtt_link.py phase <us>`).
  - An encoder thread then encodes it into four 3200 frames, with one
    Codec 2 state per run and chunks keyed to time, so a late chunk is
    dropped, never delayed.
  - The thread is preemptible (priority 0; the UI steps down to 1), so it
    can never delay the cooperative radio or data threads.
  - The soak data thread stages the chunk as soon as it is done. The TX
    record logs the chunk, its wall-clock encode time (`SOAK_F_TX_ENC`) and
    the staging lead.
- **Checking it:** `tools/c2_encode_check.py`.
  - *Encode time and staging lead:* from the TX records.
  - *Correctness:* the device's frames against `pycodec2` encoding the same
    PCM the device's way (fresh state, chunks 0, 1, 2… of the looping
    clip).
  - *Over the air:* each peer's received bytes against what the sender
    logged as sent.
- **Bench:** three units, three speech excerpts (`OSR_0010_F.pcm`,
  `OSR_0030_M.pcm`, `OSR_0011_F.pcm`, the first 8 s of each OSR file),
  +0 dBm, 1 min per setting.

## Getting the encoder fast enough

| Build | Encode per 80 ms chunk (median / max) | Note |
|---|---|---|
| 64 MHz, `-Os` | ~100 ms (max) | slower than real time: every chunk late |
| 128 MHz, `-Os` | 39–41 / 51 ms | the app core resets to 64 MHz (`HFCLKCTRL` Div2) |
| 128 MHz, `-O2` | 37–40 / 49 ms | codegen was not the cost |
| **128 MHz, `-O2`, `-fsingle-precision-constant`** | **26.5–28.3 / 29.3 ms** | the cost was software double maths |

- **128 MHz:** encode builds switch the app core to 128 MHz at boot
  (`nrfx_clock_divider_set`, as the nRF5340 audio reference does). The
  divider sets the CPU clock only; the slot timer and SPI run from the
  16 MHz peripheral clock, so TDMA timing is unchanged.
- **Single-precision constants:** upstream writes constants like
  `2.0*PI`, which promote float expressions to double, and the M33's FPU
  is single precision only. Every hot file called `__aeabi_dmul` and
  friends. Treating constants as float removed nearly all of it and cut
  the encode by a third, with a much tighter spread.
- **`-O2`** reaches the codec through a force-included pragma
  (`external/codec2_opt.h`), because Zephyr's `-Os` lands after per-target
  options.
- **Cost:** ~28 ms per 80 ms is about 35 % of the core at 128 MHz. The
  encoder stack peaks ~14.7 KB of its 20 KB.

## Results

**Correctness.** 96–97 % of chunks are bit-exact with `pycodec2`, and the
rest differ in 1–2 bits each (29–46 bits in ~190,000 per stream). The same
chunks differ on every loop of the clip, so it is deterministic float
rounding between the M33 and the host's double maths, not noise. It also
validates the Python codebook port, since a wrong table would break every
chunk. Decoded, the device stream matches the host stream's 20 ms envelope
(correlation 0.996–1.000, equal RMS).

**Over the air.** Every received chunk equals what its sender logged, byte
for byte, on every run.

**Phase sweep** (how early the chunk must be available):

| Phase | MASTER | SEC 1 | SEC 2 |
|---|---|---|---|
| 40, 36 ms | clean | clean | clean |
| 34 ms | clean; min lead 2.76 ms (0.3 ms margin) | clean | clean |
| 33 ms | 43 stale re-sends | clean | 1 |
| 32 ms | 116 stale | clean | 48 |
| 31, 30 ms | 70 to 124 stale | ~100 | ~80 to 100 |

The edge is where the model puts it: worst encode (≤ 29.3 ms), plus the data
thread's poll (≤ 2 ms), plus the 2.45 ms pickup. The master runs 1–2 ms
tighter because its clip encodes slightly slower. **36 ms** is the setting
with margin.

**Latency**, first sample of a chunk to the peer's DIO1, before decode and
playout: 80 ms (a chunk is 80 ms of audio) + 36 ms (phase) + ~8.2 ms (air)
= **~124 ms**.

## What this points to for the audio path

- **Encode per 20 ms frame, not per 80 ms chunk.** Today the encoder waits
  for the whole chunk, then encodes four frames back to back. Encoding each
  frame as it is captured leaves only the last frame (~7 ms) to do after the
  chunk completes. With the mic under our control, clocked from the frame,
  that is natural.
- **Stage from the encoder, not the polled data thread:** saves up to 2 ms.
- Together these bring the phase from 36 ms to ~12 ms, and first sample to
  peer to ~100 ms.
- **More encode speed**, if wanted: the remaining double calls (`floor` and
  `round` in `sine.c`), and kiss_fft vs CMSIS-DSP (upstream supports
  `FDV_ARM_MATH`).
- **CPU budget:** ~35 % of the app core for encode alone. Decode and
  playout on the receive side come on top.

## Reproduce

```sh
python scripts/vendor_codec2.py <codec2 1.2.0 checkout>   # already in the tree
west build ... -DCONFIG_SOAK_C2_ENCODE=y                   # both unit types
python src/tdma_console/tools/c2clip.py fetch
python src/tdma_console/tools/c2clip.py pcm soaks/clips/OSR_us_000_0010_8k.wav -o soaks/clips/OSR_0010_F.pcm
rtt_link.py --sn N clip soaks/clips/OSR_0010_F.pcm; rtt_link.py --sn N mode pcm; rtt_link.py --sn N phase 36000
rtt_link.py --all capture -o soaks/rtt/enc/ --soak 1
c2_encode_check.py soaks/rtt/enc/*.bin --pcm soaks/clips/OSR_0010_F.pcm --pcm ... --wav-dir out/
```
