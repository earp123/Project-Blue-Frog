# Task: pure-tone payload + host-side audio reconstruction

**Status:** ready for implementation. **Scope:** a compile-time alternative to the `base + i` ramp payload (a per-slot PCM tone), a META flag so logs are self-describing, a decoder tweak, and one new host script that turns a soak log into a WAV. No engine (`src/tdma/`) change. No LC3, no on-device audio.

## Why

Every RX record already carries the full 40-byte payload with `frame_ctr`, so a single unit's log holds everything needed to rebuild what its peers sent. Filling the payload with a tone instead of the ramp lets us *hear* the channel: order each peer's chunks by `frame_ctr`, sum the peers, and a missed frame becomes an audible gap. On the bench (CRC on, 0 % PER) the result will be a clean chord by construction; the value is in the field, where loss and PLC-free gaps become audible, and later with packet CRC off.

Budget reality, so nobody expects real-time audio: 40 B per 80 ms frame is 4 kbps per unit. At 8 kHz / 8-bit that is 5 ms of audio per frame, so the reconstruction is a time-compressed concatenation (16x), not real time. That is fine for a tone.

## Tone source (shared, both unit types)

New `src/tdma_console/tone_src.{c,h}`, added to CMake by name the same way `sd_log.c`/`soak_log.c` are built for both variants.

- Virtual sample rate **8 kHz**, **8-bit unsigned PCM** (silence = 128), **40 contiguous samples per transmitted frame**.
- Frequency per slot: `f = TONE_F0_HZ * (slot_id + 1)` with `TONE_F0_HZ = 330` → 330 / 660 / 990 / 1320 Hz. Harmonic, all well under the 4 kHz Nyquist, and none is an integer number of cycles per 40-sample chunk (1.65 / 3.3 / 4.95 / 6.6), so consecutive chunks differ and cross-frame phase continuity is checkable on the host.
- 32-bit phase accumulator, `delta = f * 2^32 / 8000`, 256-entry sine LUT indexed by the top 8 bits, amplitude 100 (±100 about 128). `tone_src_reset(slot_id)` at soak start; `tone_src_fill(uint8_t out[40])` advances the accumulator by 40 samples. The phase runs on across frames, so concatenating chunks on the host is seamless as long as every staged payload goes out once (`stale_retx` = 0, which it is).
- No frame counter is needed at TX time (the engine does not expose it; see Known limitations). A stale re-transmit simply repeats a chunk, which the decoder already flags as `dup`.

## Selection: Kconfig, not UI

Add `CONFIG_SOAK_PAYLOAD_TONE` (bool, default n) to the root `Kconfig`, under the app section. When set, the soak runner's TX arm/re-stage path — the ramp fill in `soak_data_step()` in `src/tdma_console/main.c` and its copy in `src/tdma_field/` — calls `tone_src_fill()` instead of writing `base + i`. Everything else in the runner is untouched.

Compile-time because a tone run is a deliberate reflash of all units anyway, and a runtime toggle would mean UI work on two front ends before we know the test is worth keeping. If it earns its place, promote it later.

## META: make the log self-describing

`struct soak_meta` is 36 of the 40 payload bytes. Use the spare four:

```c
	uint8_t  payload_mode;	/* 0 = ramp (base + i), 1 = tone */
	uint8_t  tone_fs_khz;	/* 8 */
	uint16_t tone_f0_hz;	/* 330; per-slot f = f0 * (slot_id + 1) */
```

Existing v2 files have zeros there and decode as ramp, so **no `SOAK_LOG_VERSION` bump**. Keep the `BUILD_ASSERT` on the 40-byte fit.

## Decoder

`src/tdma_console/tools/decode_soak_log.py`:

- Read the three new META fields; print `payload=tone fs=8kHz f0=330Hz` (or `payload=ramp`).
- **Skip the ramp integrity check when `payload_mode == 1`** — otherwise every tone packet reads as corrupt.
- Optional, small: for tone logs, check that each RX chunk's phase continues from the previous chunk of the same slot (fit phase to the known f, compare to the expected 40-sample advance). Report discontinuities alongside `miss`/`dup`; they should agree.

## New host script: `src/tdma_console/tools/reconstruct_tone.py`

`reconstruct_tone.py <log.bin> [-o out.wav] [--per-slot]`

1. Parse records with the decoder's parser (import it; don't duplicate the struct).
2. Refuse ramp logs (`payload_mode != 1`) with a one-line message.
3. For each RX `slot_id`: unwrap `frame_ctr` into a monotonic index (modular, same rule the decoder uses), place each 40-sample chunk at `(idx - first_idx) * 40`. Missing frames are **silence** (128); duplicates are dropped (keep first).
4. Convert to 16-bit, sum the slot streams with `1/N` scaling, write an 8 kHz mono WAV. `--per-slot` also writes one WAV per peer slot.
5. Print: per-slot frames received / missing / dup, total gap ms, and the dominant spectral peaks of the mix (expected: exactly the peers' `f0 * (slot + 1)`).

Own TX records are not mixed in: they carry no `frame_ctr` (`SOAK_F_NO_CTR`), so they cannot be placed. Standard library + `numpy` only; no new dependencies beyond what `decode_soak_log.py` uses.

## Verification

1. Build both unit types with `CONFIG_SOAK_PAYLOAD_TONE=y`; flash the three bench units. 5 min soak at +0 dBm, all three logging.
2. `decode_soak_log.py` on each card: META shows `payload=tone`, ramp check skipped, PER / miss / dup as usual (expect 0 / 0 / 0 outside any SD hole — check for the 8 KB hole first, per Known limitations).
3. `reconstruct_tone.py` on each card: listen — a clean two-tone chord with no clicks; spectral peaks only at the two peers' frequencies; gap count matches the decoder's `miss`.
4. Rebuild without the Kconfig symbol and confirm the ramp path is byte-identical to today (image size unchanged apart from the Kconfig plumbing).

Inducing loss on the bench is **not** part of this task; that is what the field campaign is for.

## Explicitly out of scope

LC3 or any codec; on-device playback; a runtime tone toggle; real-time playback; CRC-off BER runs (separate card).

## CHANGELOG

New entry under Unreleased: the tone payload option, the META fields, the decoder change and the new tool, with the 4 kbps / 16x note above so the time-compressed output is not misread. Reference this file.
