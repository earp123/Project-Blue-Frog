# Task: RTT bench link + Codec 2 transport test

**Status:** ready for implementation. **Scope:** (1) a J-Link RTT link that replaces the SD card on the bench — live soak records to the host, and a small command channel to upload audio, start/stop soaks and switch payload mode; (2) a Codec 2 payload source that transmits a pre-encoded speech clip; (3) host tools that prepare the clip, capture over RTT, and reconstruct/score the received audio. No engine (`src/tdma/`) change. No codec on the device yet — that is the next task (encode test); this one proves the transport carries Codec 2 frames intact and builds the plumbing the encode test reuses.

Read first: [`tone_payload_test.md`](tone_payload_test.md) (the payload-source pattern, the placement-by-arrival rule, card #23's first packet) and the CHANGELOG entries for the tone soak and the soak logger.

## Why these choices

**Budget.** Codec 2 mode 3200 is 64 bits per 20 ms frame = 8 B. 80 ms of speech = 4 frames = 32 B, which fits the 40 B / 80 ms L3 payload in real time with 8 B left for a header. Unlike LC3, Codec 2 needs no TDMA change. (2400 = 24 B, 1600 = 16 B are the fallbacks if the encode test finds CPU or header pressure.)

**RTT, not UART or native USB.** Both unit types already hang off the host by their J-Link USB. The TFT unit has VCOM's pins (P0.20/P0.22) taken by the breakout and builds with `CONFIG_SERIAL=n`; the shield unit uses VCOM for Zephyr logging. RTT needs no pins and no USB stack: it is a RAM ring the on-board J-Link reads and writes over SWD. Throughput on the OB J-Link is tens of KB/s; the soak stream is 3.2 KB/s at the 20 ms slot. Native USB (the DK's second connector) would mean a second cable per unit and a USB stack; UART would mean per-unit pin surgery. Neither is worth it for a bench link.

**Clip lives in RAM, uploaded once per boot.** FatFs is single-owner (the soak_log writer thread) and the TX path runs on the cooperative data thread, so reading a file from the TX path is out. A 32 KB static buffer holds ~80 s of Codec 2 3200; the clip persists across soaks until reset. The buffer size is a Kconfig so the encode test can raise it for raw PCM.

**Same record format on the wire.** RTT carries the identical 64 B `struct soak_rec` stream the card gets — META first, then RX/TX/STATS. A capture file is a soak log; `decode_soak_log.py` and `reconstruct_tone.py` read it unchanged. RTT drops show up as `seq` gaps exactly like ring drops.

**A chunk index in the header.** The tone had to fit content to place chunks and detect slips. Codec 2 bytes carry no such structure, so the 8 B header carries a per-sender chunk counter. That gives the host exact placement, dup/stale/out-of-order detection without decoding, bit-exact comparison against the source clip, and — because the sender's TX records log the same header — per-link delivery and latency by pairing TX and RX records, which the tone could not do.

## Firmware

### 1. RTT sink + control channel — `src/tdma_console/rtt_link.{c,h}`

Shared, built for both units like `soak_log.c`. Kconfig `CONFIG_SOAK_RTT` (bool, default y for both companion confs; `n` drops all of it, and the tree must still build and behave as today). Selects `CONFIG_USE_SEGGER_RTT`, `SEGGER_RTT_MAX_NUM_UP_BUFFERS=3`, `SEGGER_RTT_MAX_NUM_DOWN_BUFFERS=2`. No RTT console, no log backend change; channel 0 is left alone.

Channels, configured at init with app-owned buffers:

| Ch | Dir | Name | Size | Mode | Carries |
|---|---|---|---|---|---|
| up 1 | dev → host | `soak` | 8 KB | `NO_BLOCK_SKIP` | raw 64 B `soak_rec` stream |
| up 2 | dev → host | `ctl` | 512 B | `NO_BLOCK_SKIP` | text replies, one per line |
| down 1 | host → dev | `ctl` | 1 KB | — | text commands, plus the clip bytes |

`NO_BLOCK_SKIP` writes a record whole or not at all, so a slow or absent host can never corrupt the stream — it drops, and the drop is counted. 8 KB matches the msgq ring: ~2.5 s at the 20 ms slot, sized to swallow a host hiccup.

**Sink model.** `soak_log` gains a sink mask. The writer thread drains the ring into every active sink; SD is one sink, RTT the other. RTT is always active when built in (a record is a 64 B memcpy into the ring — negligible, and it means a UI-started soak is also captured if a host happens to be listening). SD is chosen per start: the UI start path keeps SD on (unchanged behaviour); the `soak` command defaults to RTT only. The META record goes to every sink, so an RTT capture is self-describing with no card present. SD open failure stays non-fatal, as now. `soak_log_status` gains `rtt_written` / `rtt_dropped`; show the drop count on the LOG page alongside the ring drops.

**Command channel.** Polled from the writer thread on a 10 ms `k_msgq_get` timeout (it already idles there; no new thread). ASCII line commands, `\n`-terminated; replies are single lines on up 2, `ok ...` or `err <reason>`.

| Command | Effect | Reply |
|---|---|---|
| `status` | no side effect | `ok status role=<M/S> slot=<n> sync=<state> soak=<idle/run> mode=<ramp/tone/clip> clip=<chunks>/<crc32> rtt_drop=<n>` |
| `mode ramp\|tone\|clip` | sets the payload source for the *next* soak start | `ok mode <m>` |
| `clip <nbytes> <crc32hex>` then exactly `nbytes` raw bytes | loads the clip buffer | `ok clip <nbytes> <crc32> chunks=<n>` / `err clip size` (0, > buffer, or not a multiple of 32) / `err clip crc` / `err clip busy` (a soak is running) |
| `soak <minutes> [sd]` | starts a soak via the runner's existing start path; 0 = continuous; `sd` also opens the card file | `ok soak` / `err soak busy` / `err soak nounit` (unit not picked yet) |
| `stop` | stops a running soak via the existing stop path | `ok stop` / `err stop idle` |

Rules: the unit pick stays on the front end (mandatory power-on step; not touched). A `clip` upload streams straight into the buffer as bytes arrive (`SEGGER_RTT_Read` in the poll loop); CRC32 (`crc32_ieee`) is checked on completion and a bad clip is marked invalid, so `soak` in clip mode refuses with `err soak noclip`. Unknown or malformed lines reply `err cmd`. Nothing here is reachable from the radio; it is a bench port.

### 2. Payload modes — replace the tone bool with a choice

`CONFIG_SOAK_PAYLOAD_TONE` (one day old) becomes a Kconfig choice `SOAK_PAYLOAD` with `RAMP` (default), `TONE`, `CLIP` — the mode a soak starts in. With `CONFIG_SOAK_RTT=y` all three sources are linked and the `mode` command overrides the default; without it, only the chosen source links, as before. The source is latched at soak start, never mid-run. The runner keeps one call, `payload_fill(n, out)`, dispatching on the latched mode; `tone_src` is unchanged.

### 3. Clip source — `src/tdma_console/clip_src.{c,h}`

Same contract as `tone_src`: stateless, chunk *n* is the payload for the unit's *n*-th TX slot of the run (keyed to `tx_done`), a refused submit retries the same bytes, a missed frame's audio is dropped, never delayed. Chunk *n* carries clip bytes `[(n mod chunks) × 32, +32)`, so the clip loops.

`CONFIG_SOAK_CLIP_BUF_KB` (default 32) sizes the static buffer.

**40 B payload layout (test L3 header, v0):**

```
off  len  field
 0    1   magic      0xC2
 1    1   codec      Codec 2 mode enum: 0 = 3200 (only value this task sends)
 2    2   chunk_idx  uint16 LE, n mod 65536 — per-sender, monotonic per run
 4    2   clip_id    uint16 LE, low 16 bits of the clip's CRC32
 6    2   rsvd       0
 8   32   4 × 8 B Codec 2 3200 frames = 80 ms
```

This is a *test* header, not the product L3 header — that gets designed with PTT/VAD/routing in mind. It is 8 B so the audio budget is realistic.

### 4. META

`struct soak_meta`'s `tone_fs_khz` / `tone_f0_hz` become mode-generic `p8` / `p16`:

| `payload_mode` | `p8` | `p16` |
|---|---|---|
| 0 ramp | 0 | 0 |
| 1 tone | fs kHz (8) | f0 Hz (330) |
| 2 clip | codec enum (0) | clip chunk count |

Layout and size unchanged, still no `SOAK_LOG_VERSION` bump. The clip CRC is not in META (no room); it is in every chunk header, which is where the host needs it.

### 5. Decoder

`decode_soak_log.py`: read `p8`/`p16` by mode and print `payload=clip codec=3200 chunks=<n>`; skip the ramp check for mode 2 like mode 1. Nothing else.

## Host tools (`src/tdma_console/tools/`)

### `rtt_link.py` — the bench port

`pylink-square` (pip). Connects by J-Link serial (`--sn`), `nRF5340_xxAA_APP`, SWD, `rtt_start()` with control-block autodetect. One process per DK (the J-Link DLL is one-connection-per-process); `--all` enumerates connected emulators and spawns one capture per unit.

```
rtt_link.py --sn N status
rtt_link.py --sn N mode clip
rtt_link.py --sn N clip clip_F.c2              # sends "clip <n> <crc>" then the bytes, waits for ok
rtt_link.py --sn N soak 5 [--sd]
rtt_link.py --sn N stop
rtt_link.py --sn N capture -o m.bin [--minutes M]  # drains up 1 to a file, polling every 5 ms, until stop/timeout/Ctrl-C
rtt_link.py --all capture -o soaks/rtt/          # one file per unit, named from its status line
```

`capture` writes bytes verbatim, prints record count / rate / `seq` gaps as it goes, and must be running before `soak` for a gap-free file (the 8 KB up buffer covers a late start by ~2.5 s, no more). Attaching never resets or halts the target; `west flash` still works between sessions.

### `c2clip.py` — prepare the speech

```
c2clip.py fetch                          # downloads the OSR files below into soaks/clips/ (gitignored)
c2clip.py encode in.wav -o clip.c2 [--mode 3200]   # 8 kHz 16-bit mono in; trims to a multiple of 32 B; writes clip.c2, clip_ref.wav (own decode), prints chunks + crc32
```

Speech source: the Open Speech Repository (Telchemy), Harvard sentences, already 8 kHz 16-bit PCM — Codec 2's native input. Terms: free to use, copy and publish; credit the source as "Open Speech Repository". Use three files so each unit is recognisable by ear and by `clip_id`:

| Unit | File |
|---|---|
| MASTER | `https://voiptroubleshooter.com/open_speech/american/OSR_us_000_0010_8k.wav` (F) |
| SEC 1 | `https://voiptroubleshooter.com/open_speech/american/OSR_us_000_0030_8k.wav` (M) |
| SEC 2 | `https://voiptroubleshooter.com/open_speech/american/OSR_us_000_0011_8k.wav` (F) |

Each is ~30 s of ten sentences → ~12 KB of Codec 2 3200. Commit the three `.c2` files under `soaks/clips/` with a one-line `README` carrying the attribution; do not commit the WAVs.

Codec 2 on the host: try `pycodec2` first (pip; wheels for Linux/macOS), else the `c2enc`/`c2dec` CLI from a codec2 build on `PATH`. On Windows, WSL is the least-friction route; note whichever worked in the CHANGELOG. Both scripts should use one small wrapper so the fallback is in one place.

### `reconstruct_c2.py` — place, verify, decode, score

`reconstruct_c2.py capture.bin --clip clip_F.c2 --clip clip_M.c2 ... [-o out_dir] [--tx tx_capture.bin ...]`

1. Records via `decode_soak_log.iter_records()`. Refuse anything but `payload_mode == 2`.
2. Per RX `slot_id`: validate the header (`magic`, `codec`); unwrap `chunk_idx` into a monotonic index; classify each chunk as **placed**, **dup** (same index, same bytes), **stale** (same index, different bytes — should not happen), **out-of-order**, or **foreign** (bad magic: card #23's shifted first packet lands here, and the tool should say so by recognising the shifted-by-4 pattern). Cross-check placement against arrival time (`t_us`) as `reconstruct_tone.py` does, and report any disagreement.
3. **Integrity:** match `clip_id` to a supplied clip and compare every placed chunk's 32 B to the clip at `(idx mod chunks) × 32`. Report mismatched chunks and byte error count. Bench expectation: 0 outside the foreign first packet.
4. **Delivery:** placed / missing / dup / stale / foreign per peer slot, and the missing-run length histogram (single-frame losses vs bursts).
5. **Audio:** decode each peer's frame stream with a persistent Codec 2 decoder; a missing chunk becomes 80 ms of zero PCM (no concealment — that is a later, separate topic). Write `<slot>.wav` per peer and, when `pesq` is importable, PESQ (nb) of each against that clip's `_ref.wav`, aligned by the first placed index. On a clean bench run the streams are bit-exact so PESQ is the ceiling by construction; it is there for the field.
6. **Latency (needs `--tx`):** with the sender's own capture, pair its TX records to this receiver's RX records by `(clip_id, chunk_idx)` and report `t_us(RX) − t_us(TX)` min/median/max. Both clocks are the slot clock, synced to within tens of µs, so this is a real number: stage-to-DIO1 for the air path. State clearly in the output that it excludes encode, decode and playout.

One summary block per capture, plain text, and a `--json` option for the bench log.

## Sequencing (each step is a stopping point)

1. **RTT link with the tone.** `rtt_link.c` sink + `status`/`soak`/`stop`, `rtt_link.py` `status`/`soak`/`stop`/`capture`. Run the tone soak on all three units with **no cards inserted**, captured over RTT. `decode_soak_log.py` and `reconstruct_tone.py` on the three capture files must reproduce the 2026-09-25 SD result: PER 0, 0 gaps, 0 slips, `rtt_dropped` 0. This alone retires the card from the bench.
2. **Clip path.** `mode`/`clip` commands, `clip_src`, the Kconfig choice, META `p8`/`p16`, decoder update. Bring-up check: `status` reflects an uploaded clip; a CRC-mangled upload is refused; a 1-minute clip soak captures with `payload=clip` in META.
3. **Host tools.** `c2clip.py` (fetch + encode + ref decode), `reconstruct_c2.py`. Test them first against synthetic captures built the way `reconstruct_tone.py`'s were (clean run; then injected loss, dup, stale re-transmit, out-of-order, the shifted first packet, an RTT `seq` gap) — each must land in the right bucket.
4. **Bench run.** Three units, three clips, 5 min at +0 dBm, RTT capture on all three, no cards. Then the assessment below.

## Verification

1. Step 1 acceptance as stated: the RTT-captured tone soak matches the SD one, `rtt_dropped` = 0 on every unit, and `capture` shows no `seq` gaps.
2. Ramp/tone/clip images: `SOAK_PAYLOAD=RAMP` without `SOAK_RTT` must produce the same TX path as today (tone-task diff rules apply: only `soak_log_start` and the META rename may move). Record the three image sizes.
3. Clip soak, per unit: `reconstruct_c2.py` reports, for each peer stream, 0 missing / 0 dup / 0 stale outside the known `frame_ctr` echo at run end, exactly one foreign chunk on each secondary (card #23's first packet — and none once #23 lands), and **0 mismatched chunks** against the clip. Latency block populated using the peers' captures as `--tx`.
4. Listen to all six peer WAVs: the Harvard sentences, clean, looping — female on the master's stream, male on SEC 1's, female on SEC 2's. PESQ, if available, at the ceiling.
5. Nothing induces loss on the bench. Loss behaviour is the field campaign's; the tooling above is what will read it.

## Explicitly out of scope

Codec 2 on the device (next task: encode test — same link, same header, the clip buffer holds raw PCM and the TX path encodes 4 frames per chunk, with encode time logged); on-device decode or the earpiece; any concealment; a product L3 header; unit pick over RTT; moving Zephyr logging to RTT; native USB; CRC-off runs.

## CHANGELOG

New entry: the RTT bench link (channels, commands, sink model, "the card is now optional on the bench"), the payload-mode choice replacing the tone bool, `clip_src` and the test header layout, the META rename, the three host tools, the OSR clips and attribution, and the bench result. Cross-reference the tone entry. Add to Known limitations: RTT has no host-present signal, so `rtt_dropped` climbs harmlessly whenever no capture is attached.
