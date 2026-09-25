# Task: pure-tone payload + host-side audio reconstruction

**Status:** implemented and bench-verified 2026-09-25 (results under Verification). **Scope:** a compile-time alternative to the `base + i` ramp payload (a per-slot PCM tone), a META flag so logs are self-describing, a decoder tweak, and one new host script that turns a soak log into a WAV. No engine (`src/tdma/`) change. No LC3, no on-device audio.

## Why

Every RX record already carries the full 40-byte payload, so a single unit's log holds everything needed to rebuild what its peers sent. Filling the payload with a tone instead of the ramp lets us *hear* the channel: place each peer's chunks on a common frame timeline, sum the peers, and a missed frame becomes an audible gap. On the bench (CRC on, 0 % PER) the result will be a clean chord by construction; the value is in the field, where loss becomes audible, and later with packet CRC off.

Budget reality, so nobody expects real-time audio: 40 B per 80 ms frame is 4 kbps per unit. At 8 kHz / 8-bit that is 5 ms of audio per frame, so the reconstruction is a time-compressed concatenation (16x), not real time. That is fine for a tone.

## Design rule: a bad packet costs its own frame, nothing more

This is real-time audio, so late audio is worthless. Recovery means resuming at the next good packet — never retrying, waiting, or replaying a backlog. One lost, late or garbled packet must cost exactly the frame it hit, and the next good packet must play at its own time. That rule shapes both ends:

- **TX:** a chunk is the audio for the frame it goes out in. If that frame is missed, its audio is gone; the next frame carries its own audio, not the backlog. (A generator that just advances per chunk sent is a queue: nothing is ever dropped, and every hiccup shifts all later audio later.)
- **Host:** each chunk is placed by when it arrived, independently of every other chunk. A gap is silence of exactly its length; no single bad packet can cause a good one to be discarded or misplaced.
- **No concealment** in this tool. Its job is to make loss audible, so silence is the honest rendering. Concealment (repeat, fade, LC3's PLC) is a product decision for the real audio path.

## Tone source (shared, both unit types)

`src/tdma_console/tone_src.{c,h}`, added to CMake by name next to `sd_log.c`/`soak_log.c` so both variants build it.

- Virtual sample rate **8 kHz**, **8-bit unsigned PCM** (silence = 128), **40 contiguous samples per transmitted frame**.
- Frequency per slot: `f = TONE_F0_HZ * (slot_id + 1)` with `TONE_F0_HZ = 330` → 330 / 660 / 990 / 1320 Hz. Harmonic, all well under the 4 kHz Nyquist, and none is an integer number of cycles per 40-sample chunk (1.65 / 3.3 / 4.95 / 6.6), so consecutive chunks always differ. Chunk content repeats every 20 / 10 / 20 / 5 chunks.
- **Stateless and frame-keyed.** `tone_src_fill(out, slot_id, n)` writes chunk *n* of that slot's tone: 32-bit phase `n * 40 * delta` (plain `uint32_t` wrap is exactly phase mod 2π), `delta = round(f * 2^32 / 8000)`, a 256-entry sine LUT indexed by the top 8 bits, amplitude 100 (±100 about 128).
- `n` is **the TX slot this chunk will ride**, counted from the run's start: `t->tx_done - soak.snap.tx_done` at fill time. The engine already exposes `tx_done`, so no engine change and no frame counter at TX time. Consequences:
  - a failed `tdma_tx_submit()` (`-EAGAIN`) is retried with identical bytes, because `tx_done` has not moved (the ramp is idempotent the same way, via `soak.seq`);
  - a stale re-transmit consumes a `tx_done`, so the next chunk is the one for the next frame — the missed frame's audio is simply gone;
  - while a secondary is out of sync it does not transmit and `tx_done` pauses, so its tone resumes where it stopped. That discontinuity always sits at a gap, where it is inaudible.

## Selection: Kconfig, not UI

`CONFIG_SOAK_PAYLOAD_TONE` (bool, default n) in the root `Kconfig`. When set, the soak runner's TX re-stage path in `soak_data_step()` — `src/tdma_console/main.c` and its copy in `src/tdma_field/main.c` — calls `tone_src_fill()` where it otherwise calls `fill_pattern()`. Everything else in the runner is untouched. Both call sites and the META fields use `IS_ENABLED()`, so a build without the symbol compiles the ramp path exactly as before and links no tone code (see Verification for the one unavoidable difference).

Build by adding `-DCONFIG_SOAK_PAYLOAD_TONE=y` after the unit's two files (README, "Building and flashing"). Flash every unit in the run the same way: META records only the logging unit's *own* payload mode, and the tools assume its peers match (a mixed kit shows up as ramp "bad bytes" in the decoder, or unmatched chunks in the reconstructor).

Compile-time because a tone run is a deliberate reflash of all units anyway, and a runtime toggle would mean UI work on two front ends before we know the test is worth keeping. If it earns its place, promote it later.

## Staging: why no engine change is needed

`tdma_tx_submit()` stages into a one-deep buffer that refuses a new payload while one is pending (`-EAGAIN`), which looks like oldest-wins. It is not, at frame granularity: the engine drains that buffer into the radio's TX region at **every** RX-slot entry (`rx_slot_slack_work()`), and each drain overwrites the previous one. So the newest chunk submitted before the last RX slot ahead of our TX slot is the one sent. With frame-keyed filling, one late stage costs one frame (sent as a stale repeat, which the host drops) and the next frame is on time. A chunk can only go out late if the data thread stalls across more than a frame; the reconstructor's slip count reports that.

## META: make the log self-describing

`struct soak_meta` is 36 of the 40 payload bytes. The spare four:

```c
	uint8_t  payload_mode;	/* enum soak_payload_mode: 0 = ramp, 1 = tone */
	uint8_t  tone_fs_khz;	/* 8 */
	uint16_t tone_f0_hz;	/* 330; per-slot f = f0 * (slot_id + 1) */
```

`soak_log.c` zero-fills the record, so existing v2 files have zeros there and decode as ramp: **no `SOAK_LOG_VERSION` bump**. A `BUILD_ASSERT` pins the 40-byte fit. The tone fields are set under `IS_ENABLED()`, so a ramp build still writes a byte-identical META record.

## Decoder

`src/tdma_console/tools/decode_soak_log.py` stays standard-library only:

- Record parsing moves out of `main()` into `iter_records()` (plus the existing `parse_meta()`), so other tools import it instead of duplicating the struct.
- Reads the three META fields and prints `payload=tone fs=8kHz f0=330Hz` (or `payload=ramp`).
- **Skips the ramp integrity check when `payload_mode == 1`** — otherwise every tone packet reads as corrupt.

The phase check lives in the reconstructor, which already has the tone model and the timeline.

## New host script: `src/tdma_console/tools/reconstruct_tone.py`

`reconstruct_tone.py <log.bin> [-o out.wav] [--per-slot]` (standard library + `numpy`; numpy is new here, the decoder does not need it).

1. Parse with the decoder's `iter_records()` / `parse_meta()`. Refuse ramp logs (`payload_mode != 1`) with a one-line message.
2. **Log hygiene first** (the 8 KB hole, CHANGELOG "Known limitations"). The real holes hold zeros, 0xFF, random bytes and stale sectors of other runs, and random bytes can decode as a plausible RX record, so no per-record check is safe. In a healthy file `seq − file index` is one constant that only steps up at a ring drop, so the tool splits the file into runs of constant offset and trusts the run holding record 0 plus every run of ≥ 256 records (twice the hole), then admits a shorter run only if its offset and uptime sit between two trusted neighbours (or, at the end of the file, match the last one). RX "from" the logger's own slot is ignored. Every discarded stretch and every `seq` jump is a *log break*; a gap spanning one is reported as log loss, not radio loss.
3. **Place by arrival time, not `frame_ctr`.** For each RX record, `τ = t_us − slot_id · slot_us` removes the slot's offset within the frame, so every packet of one frame has the same τ. `t_us` is the free-running 1 MHz slot clock (sync corrections move only the next compare, never the counter), so walk the records in order and advance a shared frame index by `round(Δτ / period)`, with `Δτ` taken mod 2^32 (the counter wraps every 71.6 min) and `period` the measured median frame spacing (absorbs the receiver's crystal error over long outages). Why not `frame_ctr`: secondaries don't count, they echo the master's counter from its last beacon (`tdma_core.c:108`; only the master increments). A secondary that misses a beacon sends a *fresh* payload under a *repeated* counter, which reads as dup + miss — a keep-first rule would drop a good chunk and leave a false gap. `frame_ctr` is still reported as a cross-check.
4. **Duplicates are repeats, not counter matches:** a chunk identical to its slot's previous accepted chunk one frame later (a stale re-transmit), or a second chunk in the same frame (one TX per slot per frame, so that can only be a double-logged record). Tone content repeats every 5–20 chunks, so identical content further apart is legitimate.
5. **Content check.** The host regenerates the tone with the firmware's integer math, so each chunk identifies its content index mod its period (nearest match; a chunk far from every expected one is *unmatched*, e.g. card #23's garbled first packet after lock). Between consecutive frames the index must advance by exactly one; a disagreement is a **slip** — audio that went out late or out of order. Expect 0.
6. Missing frames are silence (128). Convert to 16-bit, sum the peer streams with `1/N` scaling, write an 8 kHz mono WAV (default: next to the log). `--per-slot` also writes one WAV per peer.
7. Print per peer slot: chunks placed / dup / unmatched / slips, gaps split radio vs log with their length in both audio time (5 ms/frame) and air time (80 ms/frame), `frame_ctr` disagreements; then the dominant spectral peaks of the mix (expected: exactly the peers' `f0 · (slot + 1)`).

Own TX records are not mixed in: they carry no RX timestamp (`t_us = 0`, `SOAK_F_NO_CTR`), so they cannot be placed on the receive timeline.

## Verification

All four steps passed (bench results below the list).

1. Build both unit types with `CONFIG_SOAK_PAYLOAD_TONE=y`; flash the three bench units. 5 min soak at +0 dBm, all three logging.
2. `decode_soak_log.py` on each card: META shows `payload=tone`, ramp check skipped, PER / miss / dup as usual (expect 0 / 0 / 0 outside any SD hole — check for the 8 KB hole first).
3. `reconstruct_tone.py` on each card: listen — a clean two-tone chord with no clicks; spectral peaks only at the two peers' frequencies; slips 0; radio gaps match the decoder's `miss` (outside a hole).
4. Rebuild without the Kconfig symbol and confirm the TX path is unchanged. Done 2026-09-25 against pristine builds of the tree before this change: the ramp images differ only in `soak_log_start()` (+32 B, TFT 103,064 → 103,096 B, shield 117,460 → 117,492 B), which now zero-fills and copies a 40-byte `soak_meta` instead of 36. The META record written to the card is byte-identical, since those four bytes were already zero, and no tone code is linked. Tone images: TFT 103,384 B, shield 117,780 B; the `sine_lut` and `phase_delta` tables read back from the tone ELF match the reconstructor's model exactly.

Host side, done 2026-09-25:
- The decoder's output on every existing ramp log is unchanged apart from the new `payload=ramp` label; its CSVs are byte-identical.
- The reconstructor was run against synthetic logs whose sender chunks come from a running phase accumulator over the LUT parsed from `tone_src.c`. A clean run reconstructs both peers bit-exact. A faulted run injects radio loss, a realistic hole (zeros, 0xFF, random bytes, stale sectors), a ring drop, a stale re-transmit, a secondary's missed beacon (`frame_ctr` repeat with a fresh payload), a sender out of sync and card #23's garbled first packet; each one lands where the rules above say, and the fresh chunk under the repeated counter is kept.
- On the real ramp logs (`soaks/P0_5M_00{0,2,3}.BIN`, `docs/P0_30M_000.BIN`), arrival-time placement agrees with `frame_ctr` at every boundary, and hygiene discards exactly the 128 hole records in the two files that have one. The resulting gaps (42 and 41 frames in SEC 1's file) match the CHANGELOG's account of that hole and are attributed to the log.

### Bench results (2026-09-25)

Three units, 5 min at +0 dBm, all on the tone build, logs in `soaks/`: MASTER `P0_5M_004.BIN`, SEC 1 `P0_5M_003.BIN`, SEC 2 `P0_5M_001.BIN`.

| Log | Decoder | Reconstructor | Peaks |
|---|---|---|---|
| MASTER | PER 0.000 %, 0 missed / 0 dup | slot 1: 3,743 placed · slot 2: 3,734 — 0 gaps, 0 dup, 0 slips, 1 unmatched each | 660, 990 Hz |
| SEC 1 | PER 0.000 %, 0 missed / **4 dup** | slot 0: 3,746 · slot 2: 3,738 — 0 gaps, 0 dup, 0 slips; slot 2: 1 unmatched | 330, 990 Hz |
| SEC 2 | PER 0.000 %, 0 missed / **4 dup** | slot 0: 3,737 · slot 1: 3,741 — 0 gaps, 0 dup, 0 slips | 330, 660 Hz |

Engine counters on every unit: `stale_retx`, `slot_timeouts`, `busy_timeouts` all 0; one `rx_crc_err` each. No SD hole in any file. Apart from card #23's first packet, every stream is phase-continuous for the whole run. Two things the tone showed that the ramp could not:

- **The 4 "dups" are the `frame_ctr` echo, caught on air.** They are the last 4 frames of each secondary's log, on the *other* secondary's stream. The master's 5 min ended first, at counter 3750; the secondaries, no longer hearing beacons, kept transmitting fresh audio under the frozen 3750 for 4 more frames. The decoder counts them as duplicates; the content says they are new chunks (0 content dups, 0 slips). This is the bias the placement-by-arrival rule exists for; the decoder and on-device counters still have it (follow-up).
- **Card #23's garbled first packet is the previous received packet, shifted by 4.** The one unmatched chunk per stream is each secondary's first packet after lock. Its payload is byte-exact `[received header (4 B)] + [received payload bytes 0–35]`: SEC 1's first packet carries the master's counter-8 beacon, and SEC 2's carries SEC 1's counter-15 packet. So a received packet was written at radio buffer offset `0x04`, the start of the TX payload region. That is where a fourth consecutive RX lands if the RX pointer advances instead of resetting: TX base `0x00`, RX base `0x80`, 44-byte packets, `0x80 + 3 × 44 = 0x104`, which wraps to `0x04`. This is consistent with card #23's wrap prediction. With the ramp, a received ramp shifted by 4 bytes is still a valid ramp, so only header bytes 0/2/3 looked wrong; the tone's frequency identifies whose data it is.

Inducing loss on the bench is **not** part of this task; that is what the field campaign is for.

## Explicitly out of scope

LC3 or any codec; on-device playback; concealment of any kind; a runtime tone toggle; real-time playback; CRC-off BER runs (separate card).

## CHANGELOG

New entry under Unreleased: the tone payload option, the META fields, the decoder change and the new tool, with the 4 kbps / 16x note above so the time-compressed output is not misread. Reference this file.
