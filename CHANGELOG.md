# Changelog

All notable changes to this firmware are documented here. The format is loosely
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project is
pre-release, so the current development state lives under **Unreleased** until the
first tagged version.

For hardware wiring, build/flash instructions, and SDK setup, see
[README.md](README.md).

## [Unreleased]

On-device radio-evaluation tooling for the nRF5340 DK + Wio-SX1262 (SX1262),
plus the first slice of the wireless-intercom firmware (TDMA radio layer).
_Last updated: 2026-09-25._

### Firmware variants

Two variants, one per unit type on the bench, build from one source tree.
Exactly one `main()` is linked, chosen by the Kconfig choice in
[`Kconfig`](Kconfig) through the unit's companion conf. The README's
"Building and flashing" has both build lines.

- **TFT unit** (`CONFIG_APP_TDMA_CONSOLE`) —
  [`src/tdma_console/`](src/tdma_console) + the [`src/tdma/`](src/tdma)
  engine, built with `boards/nrf5340dk_nrf5340_cpuapp_tft.{overlay,conf}`.
  See "TDMA field console" below.
- **Shield unit** (`CONFIG_APP_TDMA_FIELD`) —
  [`src/tdma_field/`](src/tdma_field) + the same engine, built with
  `boards/nrf5340dk_nrf5340_cpuapp_shield.{overlay,conf}`. See "Rev 2 shield /
  field variant" below.

Four other variants were retired on 2026-09-25: LoRa send, the telemetry
console, the TDMA UART-shell test and the UART soak harness (see "Streamlined
to two unit types"). Their sections below are kept as history.

### Staging lead sweep: the engine's real payload deadline (2026-09-25)

Where a payload must reach the engine to make its frame, and what that does
to latency. Write-up: [`docs/stage_lead_sweep.md`](docs/stage_lead_sweep.md).

- **Guaranteed deadline: 19.6 ms before a unit's TX boundary.** It is the
  RX-slot entry of the slot before ours, less ~0.4 ms of `SetRx`, and holds
  for every unit whatever its neighbours do. The master (whose preceding
  slot 3 is empty) shows it as a sharp cliff.
- **The bench-only deadline: 10.3 ms.** It applies only in frames where the
  preceding slot's packet is heard (its RxDone at 11.7 ms, plus ~1.4 ms to
  drain it). Frames where it wasn't heard missed at 15.7–18.7 ms. Don't
  design to it: in the field every lost preceding packet is a miss.
- **Stage-to-DIO1 is the lead + ~8.2 ms:** 29.2 ms at a 22 ms lead, against
  78.7 ms when the runner stages right after TxDone.
- **Consistently late costs +80 ms and no loss; intermittently late loses
  chunks and re-sends the previous one.** Jitter across the deadline is the
  failure to avoid.
- **Engine:** new public `tdma_next_tx_us()`, the next TX boundary in
  slot-clock time, from a frame anchor published at every slot tick.
- **Runner** (RTT builds only; the TX path without RTT is still
  instruction-identical to `9d9a1a8`): the `lead <us>` command stages each
  chunk that long before the boundary, and the TX record logs the lead
  achieved (`SOAK_F_TX_LEAD`).
- **Tools:** `rtt_link.py lead`, and `tools/lead_sweep.py` to run a sweep
  and classify every chunk as on time, late or lost against its lead.
  `reconstruct_c2.latency_by_chunk()` gives per-chunk latency.
- **Next, engine work:** take the payload at a point just before our own TX
  (in the TX-slot entry, or on a pre-TX alarm). That would cut the
  guaranteed lead from one slot to ~1–2 ms and stage-to-DIO1 to ~10 ms, the
  same for every unit.

### RTT bench link + Codec 2 transport test (2026-09-25)

The card is now optional on the bench. Each unit streams its soak records to
the host over J-Link RTT and takes bench commands on the same link, and a soak
can carry a pre-encoded Codec 2 speech clip. Task and design:
[`docs/rtt_link_c2_transport.md`](docs/rtt_link_c2_transport.md). This
follows the tone option below and reuses its placement rules.

- **Bench-verified: three units, three speech clips, 5 min at +0 dBm, no
  cards, RTT capture on all three.** Captures are in `soaks/rtt/c2_5m/`
  (untracked).
  - PER is 0.000 % on every unit. `rtt_dropped` is 0 and every capture has
    0 `seq` gaps.
  - `reconstruct_c2.py` reports 0 missing, 0 dup, 0 stale and 0
    out-of-order on all six peer streams. Every placed chunk is byte-exact
    against its sender's clip: 0 mismatched chunks. `--tx` pairing agrees:
    every chunk each unit sent in the common span arrived.
  - The only foreign chunks are each secondary's first packet (card #23),
    seen by both of its peers. See Known limitations for the second layout
    this run found.
  - Stage-to-DIO1 latency is 78.7 ms median (77.6 to 79.7 ms) on every
    link. The one outlier is the master's chunk 0 at 98 ms, staged before
    its first TX slot. A payload waits almost a whole frame because the
    runner stages the next chunk right after its own TxDone. The figure
    excludes encode, decode and playout.
  - Each peer WAV tracks its clip's reference decode: 20 ms envelope
    correlation 0.996 to 0.998, equal RMS, no silent frames. The samples are
    not identical, because Codec 2 synthesises unvoiced sound with random
    phases, so two decodes of the same frames differ. PESQ was not run: the
    `pesq` package needs a C compiler to install on this host.
  - The step 1 check (the 2026-09-25 tone soak, repeated over RTT with no
    cards) matched the SD result: PER 0, 0 gaps and 0 slips on all six
    streams, `rtt_dropped` 0.
- **RTT bench port** (`src/tdma_console/rtt_link.{c,h}`, both units,
  `CONFIG_SOAK_RTT`, default y). Up 1 carries the same 64 B records the card
  gets, META first, so a capture file is a soak log and every decoder reads
  it unchanged. Up 2 carries one reply per command. Down 1 carries commands
  (`status`, `mode`, `clip`, `soak <min> [sd]`, `stop`) and clip bytes. The
  up channels are `NO_BLOCK_SKIP`, so a missing host drops whole records
  (counted) and never corrupts the stream. Commands are read on the
  `soak_log` writer thread. `status`, `soak` and `stop` are carried out by
  the unit's UI loop, since stopping a soak waits on the writer. Zephyr's
  RTT log backend is forced off, so channel 0 and UART logging are
  unchanged. With `n`, the tree builds as before.
- **Sinks.** `soak_log` writes every record to RTT when it is built in, and
  to the card when the run asks for it. A UI start keeps the card; the
  `soak` command adds it only with `sd`. A card that fails to open no longer
  stops an RTT run. The LOG page shows RTT drops (`r`) next to ring drops
  (`d`).
- **Payload modes.** `CONFIG_SOAK_PAYLOAD_TONE` is now one option of the
  choice `SOAK_PAYLOAD` (ramp default, tone, clip), so existing build lines
  still work. With RTT all three sources are linked and `mode` picks the next
  soak's. The runner latches it at start and makes one `payload_fill()`
  call (`payload_src.h`).
- **Clip source** (`src/tdma_console/clip_src.{c,h}`). A 32 KB RAM buffer
  (`CONFIG_SOAK_CLIP_BUF_KB`) is loaded once per boot with
  `rtt_link.py clip`. Each payload is an 8 B test header (magic 0xC2, codec
  0 = 3200, chunk index, clip id = CRC32 low 16 bits) plus four Codec 2 3200
  frames: 80 ms of speech. Chunk *n* is keyed to `tx_done` like the tone,
  and the clip loops. Uploads are refused during a soak, on a bad size or
  CRC, and after a 2 s stall. A clip soak with no clip loaded is refused.
- **META:** `tone_fs_khz` / `tone_f0_hz` became mode-generic `p8` / `p16`.
  For clip they hold the codec and the chunk count. Same bytes, so there is
  no version bump, and tone logs decode as before.
- **Latency needed a firmware addition.** The task assumed TX records carry
  a time, but they logged `t_us = 0`. Each unit's slot clock is also its own
  free-running counter, so times from two units cannot be subtracted
  directly. The engine gained one public getter, `tdma_now_us()`, a wrapper
  around `tdma_port_now()`. With RTT the runners log the stage time in each
  TX record (new flag `SOAK_F_TX_T`). `reconstruct_c2.py` measures the clock
  offset between two units by common view: both hear a third unit's chunk
  *k*, and the difference of their two RX times is the offset. This needs
  three units.
- **A ramp build without RTT keeps the pre-task TX path.** `soak_data_fn` is
  instruction-identical to `9d9a1a8` on both units (150 instructions). The
  image sizes are below:

  | Unit   | Ramp, no RTT | With RTT (ramp, tone or clip default) |
  |--------|--------------|---------------------------------------|
  | TFT    | 103,288 B    | 107,428 B                             |
  | Shield | 117,652 B    | 122,216 B                             |

  RTT adds 32 KB of RAM for the clip buffer and 9.5 KB for the RTT rings.
- **Host tools** (`src/tdma_console/tools/`):
  - `rtt_link.py` (pylink-square): `status`, `mode`, `clip`, `soak`,
    `stop` and `capture`. `--all` runs one process per J-Link. `capture
    --soak M` starts the run once it is draining and ends when the run does.
  - `c2clip.py`: `fetch` downloads the three Open Speech Repository files,
    and `encode` writes a `.c2` clip plus its `_ref.wav` decode.
  - `c2codec.py`: the one Codec 2 wrapper both tools use. It tries
    `pycodec2` first, then `c2enc`/`c2dec` on PATH. `pycodec2` has a Windows
    wheel for CPython 3.13, so no WSL is needed.
  - `reconstruct_c2.py` does the following:
    - checks each chunk's header and names card #23's packets;
    - places chunks by unwrapped chunk index into placed, dup, stale,
      out-of-order and foreign;
    - cross-checks placement against arrival time;
    - compares every chunk byte for byte with the clip;
    - prints a missing-run histogram that separates log loss from radio
      loss;
    - writes a WAV per peer, with gaps as silence;
    - with `--tx`, pairs each link's TX and RX records for delivery and
      latency, and optionally writes `--json`.
  - `test_reconstruct_c2.py`: 12 synthetic cases, all passing.
- **Clips:** `soaks/clips/OSR_0010_F.c2` (MASTER), `OSR_0030_M.c2` (SEC 1)
  and `OSR_0011_F.c2` (SEC 2): 420, 586 and 409 chunks. They are Harvard
  sentences from the Open Speech Repository, credited in
  `soaks/clips/README`. The WAVs are gitignored.
- **`reconstruct_tone.trusted_records` fix:** a short final run after a
  forward `seq` jump (a ring or RTT drop near the end) is now kept instead
  of being discarded as an SD hole. Output is unchanged on every existing
  log, including `P0_30M_000`'s 128-record hole, and garbage tails are
  still discarded.

### Tone payload option + audio reconstruction (2026-09-25)

A soak can now carry audio: build with `CONFIG_SOAK_PAYLOAD_TONE=y`, and any
unit's log turns into a WAV of what its peers sent, so a lost frame is an
audible gap. Task and design:
[`docs/tone_payload_test.md`](docs/tone_payload_test.md).

- **Bench-verified: three units, 5 min, +0 dBm.** Logs are in `soaks/`.
  - PER is 0.000 % on every unit, and every peer stream reconstructs with
    0 gaps and 0 slips. Each mix peaks at exactly its two peers' tones.
  - Each secondary's decoder reports 4 "dups". These are the `frame_ctr`
    echo caught on air: the master's run ended first, and the secondaries
    kept sending fresh audio under its frozen counter. The content shows
    they are new chunks.
  - The tone also pins down card #23's garbled first packet: it is the
    previous received packet, shifted into the TX payload at buffer offset
    `0x04` (see Known limitations).

- **The WAV is not real time.** 40 B per 80 ms frame is 4 kbps per unit: at
  8 kHz / 8-bit that is 5 ms of audio per frame, so the WAV is the run
  compressed 16x. A 5 min soak is 18.75 s of audio.
- **Tone source** (`src/tdma_console/tone_src.{c,h}`, built for both units).
  Slot *s* sends 330 Hz × (*s* + 1), 40 samples per frame. It keeps no
  state: chunk *n* is the audio for the unit's *n*-th TX slot of the run,
  keyed to the engine's `tx_done`. A refused submit is retried with the same
  bytes, and a missed frame's audio is dropped, never delayed. The rule
  throughout is that a bad packet costs its own frame and nothing more.
- **No engine change.** The one-deep stage buffer looks oldest-wins, but the
  engine drains it into the radio at every RX-slot entry and each drain
  overwrites the last. So the newest chunk submitted before the last RX
  slot ahead of our TX slot is the one sent.
- **META is self-describing.** The four spare bytes of `struct soak_meta`
  now hold `payload_mode`, `tone_fs_khz` and `tone_f0_hz`. Older v2 files
  have zeros there and read as ramp, so there is no `SOAK_LOG_VERSION` bump.
  A ramp build still writes the same META record; its image differs from
  before only in `soak_log_start()` (+32 B for the larger struct copy) and
  links no tone code. Tone images: TFT 103,384 B, shield 117,780 B.
- **Decoder:** prints `payload=ramp|tone`, and skips the ramp integrity
  check for tone logs, where every packet would otherwise read as 40 bad
  bytes. Parsing moved into `iter_records()` so other tools can import it.
  Output on existing logs is otherwise unchanged.
- **New `tools/reconstruct_tone.py`** (needs numpy). It places each chunk by
  arrival time (`t_us`), not `frame_ctr`: secondaries only echo the master's
  counter, so one missed beacon would make a counter-based tool throw away a
  good packet. Duplicates are byte-identical repeats. Gaps are silence,
  split into radio vs log loss. It checks each chunk's content against the
  tone to count *slips* (audio late or out of order), and prints the mix's
  spectral peaks.
- **The tool screens out the 8 KB hole.** It trusts only long runs of
  constant `seq − index`. On the real logs it discards exactly the 128 hole
  records, and arrival-time placement agrees with `frame_ctr` at every
  boundary.

### Streamlined to two unit types (2026-09-25)

The bench now runs two kinds of radio, and the tree builds exactly those:

| Unit | Firmware | Files |
|---|---|---|
| TFT unit | `CONFIG_APP_TDMA_CONSOLE` | `boards/nrf5340dk_nrf5340_cpuapp_tft.overlay` + `.conf` |
| Shield unit | `CONFIG_APP_TDMA_FIELD` | `boards/nrf5340dk_nrf5340_cpuapp_shield.overlay` + `.conf` |

- **Removed:**
  - LoRa send (`src/main.c`).
  - The telemetry console: `src/console.c`, `radio_cfg`, `payload`,
    `pingpong` and `boards/..._display.conf`.
  - The TDMA UART-shell test (`src/tdma_app/`).
  - The UART soak harness (`src/soak/`).

  Their Kconfig entries went with them, including the TDMA test's
  role/slot/autostart options and the soak-test choice. Everything stays in
  git history.
- **One overlay + one conf per unit, with matching names.** The TFT pair was
  `display.overlay` + `tdma_console.conf`, and some builds also stacked the
  old telemetry console's `display.conf` on top. It is now `_tft.overlay` +
  `_tft.conf`.
- **A mismatched build stops at CMake.** If the unit's conf and overlay
  don't match, or are missing, the build fails with the two correct build
  lines rather than failing later in the link.
- **Shared code is built for both units.** The TFT-only `ui_widgets` and
  `touch_cal` moved into `src/tdma_console/`. The SD and soak loggers stay
  there too, and CMake builds them for both units.
  `LORA_SX126X_NATIVE_SLEEP` is now `n` unconditionally, and `prj.conf` only
  holds settings both units share.
- **No firmware change:** pristine builds of both units produce images the
  same size as before (TFT 103,064 B, shield 117,460 B).
- **README rewritten** around the two units: build and flash lines, the
  power-on unit pick, the rev 2 shield pins, and the SD hot-swap caveat.

### Rev 2 shield / field variant (2026-09-24)

The rev 2 shield carries the whole front end: SX1262 radio, a 0.96"
SSD1306 128×64 OLED and a push-push microSD socket. The TFT breakout is not
used. New variant `CONFIG_APP_TDMA_FIELD`; the TFT console is unchanged
apart from the unit pick. Its overlay/conf pair was renamed `_tft` on
2026-09-25.

| Device | Bus | Pins (DK Arduino header) |
|---|---|---|
| Wio-SX1262 | spi2 | unchanged: SCK P1.05, MOSI P1.04, MISO P1.01, CS P1.07, RST P1.06, BUSY P1.09, DIO1 P1.08, RF-SW P1.00 |
| microSD | spi4 | SCK **P1.14** (D12), MOSI P1.13 (D11), MISO **P1.15** (D13), CS P1.11 (D9), DET P1.12 (D10) |
| SSD1306 128×64 | i2c1 @ 400 kHz | SDA P0.25 (A4), SCL P0.26 (A5), RST P0.07 (A3), addr 0x3C |
| DK buttons / LEDs | gpio | board defaults; LEDs P0.28–31 are free again |

- **Pins traced in the fabrication data.** The two facts the task flagged as
  unverified were checked against the rev 2 board's ODB++ netlist
  ([`Hardware/OLED-SD-SX1262-Shield_2026-08-16.zip`](Hardware/OLED-SD-SX1262-Shield_2026-08-16.zip)).
  - **SD:** D12 → the socket's `SCLK` and D13 → `DO/DAT0`. SCK is P1.14 and
    MISO is P1.15, the reverse of the DK's default `arduino_spi`.
  - **OLED:** the header's SDA/SCL run to A4/A5 (P0.25/P0.26). The R3
    header's dedicated SDA/SCL pins (P1.02/P1.03) are unconnected.
  - **Radio:** D0–D7 carry the same nets as rev 1.

  All three were then confirmed on the bench (table below).
- **Netlist findings that affect bring-up:**
  - **DET:** the socket's `CD1` pad has a 100 k pull-up (R3) and a
    capacitor to GND. Its `CD2` pad is unconnected, but the switch still
    pulls `CD1` low when a card is seated, so DET works as-is.
  - **I2C pull-ups:** the shield has none on SDA/SCL. The overlay enables
    the nRF's internal pull-ups (~13 k) as a fallback. That is too weak for
    fast-mode rise times if the OLED module carries no pull-ups of its
    own; in that case drop to `I2C_BITRATE_STANDARD`.
  - **OLED header (JP1):** pin order is SDA, SCL, RST, GND, n/c, VCC.
- **Overlay** [`boards/nrf5340dk_nrf5340_cpuapp_shield.overlay`](boards/nrf5340dk_nrf5340_cpuapp_shield.overlay),
  passed via `EXTRA_DTC_OVERLAY_FILE`:
  - `spi4` re-pinned, with the SD slot on CS 0. The `sdhc`/`mmc` values are
    carried over from the display overlay: 4 MHz and `power-delay-ms =
    <250>`, the 2026-07-31 enumeration fix.
  - `i2c1` re-pinned, with the SSD1306 node from Zephyr's `ssd1306_128x64`
    shield plus the reset line. It also sets `zephyr,concat-buf-size =
    <1025>`. The SSD1306 driver sends each frame as a 1-byte control prefix
    plus 1024 pixel bytes, and TWIM has to merge the two in a RAM buffer
    that defaults to 16 bytes. With the default, the controller ACKed its
    init but every frame write failed and the screen stayed blank (+1 KB
    RAM).
  - `zephyr,user` gains `sd-det-gpios`, which the app reads; the SD driver
    does not.
- **Conf** [`boards/nrf5340dk_nrf5340_cpuapp_shield.conf`](boards/nrf5340dk_nrf5340_cpuapp_shield.conf):
  the TDMA console's SD/FAT block (exFAT, LBA64, `FS_FATFS_MOUNT_MKFS=n`),
  CFB, gpio-keys, a 2 KB heap for CFB's frame, and no `MIPI_DBI`. **UART
  logging stays on**, since this shield does not touch VCOM.
- **Soak runner** copied from the TFT console as-is: `soak_start` /
  `soak_finish` / `soak_data_step` and the `K_PRIO_COOP(6)` data thread.
  Logs always go to the card root. `sd_log.c`/`soak_log.c` are shared, not
  copied: CMake adds the two files by name, and their guards now accept
  either variant.
- **UI: a cycle menu.** One item per screen, drawn as large as it fits.
  The first cut packed 8 rows of 8×8 text onto the panel, which was
  unreadable on a 0.96" OLED. Each screen is now a caption line, one middle
  item and a detail line. B1/B2 cycle, B3 (OK) acts, B4 goes back.
  - **UNIT** at boot is mandatory: MASTER, SEC 1, SEC 2 or SEC 3 (see
    "Unit pick" below). The selection starts on SEC 1.
  - **HOME** cycles SOAK, TX PWR and SD CARD. The caption line shows the
    unit. Under SOAK, the detail line shows the card state (`SD 2 logs`,
    or `SD disk -116`), rescanned on every entry to HOME.
  - **SOAK** cycles 5 MIN, 30 MIN, 2 HOURS and CONTINUOUS. While a run is
    live, UP/DOWN cycle pages at 2 Hz: TIME, RX (miss/dup), TX (stale),
    PHASE (ppm), LINK (RSSI/SNR), LOG (file and records, or `LOG FAIL` with
    `<stage> <errno>`) and LAT (`N/51` ms). The caption shows run state and
    unit (`RUN M`, `SYNC S2`). B4 stops a run.
  - **TX PWR:** OK to edit, −9…+22 dBm, default **+0 dBm**.
  - **SD CARD** pages: `.BIN` count with the newest log, free/total space,
    and card detect (`CARD IN` / `NO CARD` with the raw level).

  UART logs boot, role, TX power, every card scan, every DET change, and
  a one-line summary of each run.
- **Card summary is a new writer-thread op**, `sd_fsop_submit_summary()`,
  rather than `SD_FSOP_LIST`. LIST stops at the caller's buffer and sorts
  by name. FAT appends new entries at the end of the directory, so a
  capped list drops exactly the newest logs. The new op walks the whole
  directory, counts `*.BIN`, keeps the last one in directory order, and runs
  `fs_statvfs`. A soak cannot start while a scan is in flight: the scan
  shares the writer thread with the log's open.
- **Fonts.** The caption and detail lines use CFB's 10×16 font, which fits
  12 characters, and every fixed string is written to fit that. The middle
  item takes the tallest font that fits, capped at 24 px so it stays clear
  of the edge lines. [`src/tdma_field/font_unscii8.c`](src/tdma_field/font_unscii8.c)
  adds an 8×8 font, unscii-8 (public domain), converted mechanically from
  LVGL's copy in the NCS tree by
  [`tools/gen_font_unscii8.py`](src/tdma_field/tools/gen_font_unscii8.py).
  It is now only a fallback for a runtime value too long for 12 characters.
- **Shared logger fix:** a write or close failure left `err_stage` blank, so
  the screen showed a bare errno. Those paths now report `write` / `close`,
  and each run starts with a clean stage. This affects the TFT console too.
- Build (FLASH 117,292 B / RAM 45,756 B):
  `west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-shield --`
  `-DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.overlay`
  `-DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.conf`

**Unit pick (both consoles).** Until now the TX slot followed the role:
master in slot 0, every secondary in slot 1. With three units, two
secondaries would transmit together in slot 1. The master would see
collisions, and the two secondaries would never hear each other. The
power-on role pick is now a **unit** pick, one choice per slot: MASTER
(slot 0) or SEC 1–3. It sets `tdma_config.slot_id`, the log's META slot and
the TX records' slot. The engine already accepted any slot 0–3; only the
master has to be in slot 0, because secondaries lock to its slot-0 beacon.
Order among secondaries does not matter as long as each has its own slot.
- **Field unit:** a cycle item (`UNIT 2/4`, `SEC 1`, `slot 1  OK`).
- **TFT console:** four touch/button choices under SELECT UNIT; buttons
  shrink from 90 to 48 px to fit.
- Both still let two units pick the same slot; that is on the operator.
- Three units stay under card #23's receive-buffer wrap. Each unit hears at
  most two packets between its own transmissions, and 2 × 44 B from 0x80
  ends at 0xD7. The spill onto the TX header needs a third, i.e. four units.

**Bench bring-up (2026-09-24, one rev 2 unit as MASTER, UART on VCOM):**

| Step | Pass | Result |
|---|---|---|
| OLED | splash, then ROLE | **Pass.** ACKs at 0x3C on P0.25/P0.26 at 400 kHz; no fallback needed. Needed the concat-buffer fix above. |
| SD | card mounts, count + free shown | **Pass** with the fab-data pins (SCK P1.14 / MISO P1.15), 4 MHz. The swapped mapping fails `CMD0`. |
| DET | raw level with / without a card | **Pass, active-low.** Raw 0 seated, raw 1 out, over two pull/reinsert cycles. |
| Logging | a soak writes a complete file | **Pass.** `P0_5M_000`: 744 records, 0 dropped. `P0_5M_001`: 2,001 records, 0 dropped. 0 write errors. |
| Radio | 5 min two-unit soak at +0 dBm: `slot=20000us` both cards, lock in single-digit seconds, PER 0.000 %, 0 missed / 0 dup, `dtx` ≈ 8292 µs | **Pass, with three units** (2026-09-25, below). |

Two bench lessons:

- **Early `CMD0` failures were an unseated card.** The push-push socket
  makes no contact until it clicks. DET reads raw 1 until then, so check
  DET before touching the SD pins.
- **Swapping the card while mounted breaks logging until reboot.** DET is
  read by the app only; the SD stack never re-initialises the card, so every
  write fails with -5. A remount on a DET change (unmount,
  `DISK_IOCTL_CTRL_DEINIT`, mount) would fix it; it is not built.

### First three-unit soak (2026-09-25)

The first run with more than two units: 5 min at +0 dBm and 20 ms slots,
all three logged to SD, using the unit pick above.
- **MASTER** (slot 0) and **SEC 1** were the two TFT consoles.
- **SEC 2** was the rev 2 field unit, since its log is that card's third
  file.
- All three files carry `META slot=20000us` and the right role and slot.

| Signal | Bar | MASTER | SEC 1 | SEC 2 |
|---|---|---|---|---|
| Lock | single-digit seconds | — | 1.35 s | 2.79 s |
| RX from each peer | PER 0.000 %, 0 missed/dup | slot 1: 3,734 · slot 2: 3,715, both **0 / 0** | 0 / 0 outside a log hole (below) | slot 0: 3,719 · slot 1: 3,715, both **0 / 0** |
| `evt_dt` median | 8292 ± tens of µs | 8,278 (8,221–8,328) | 8,299 (8,238–8,335) | 8,305 (8,248–8,343) |
| `phase_err` p95 / worst | ≤ 50 / < 125 µs | — | 35 / 52 µs | 33 / 51 µs |
| Frame spacing | 80 ms | 80,000 ± 14–22 µs | 80,000 ± 14–20 µs | 80,000 ± 19–21 µs |
| `stale_retx` / `slot_timeouts` / `busy` / CRC | all 0 | all 0 | all 0 | all 0 |

`ppm` means were +12.9 and −22.7, with σ 245–252. That matches the
acceptance soak and card #24's phase-jitter reading.

- **SEC 1's log has the 8 KB hole: a third occurrence.** Records
  2168–2295 are file sectors 271–286: 128 records, 16 sectors,
  sector-aligned, 40 zeroed and 88 stale. That is the same size and the
  same zeroed/stale mix as the acceptance-soak hole. A naive decode reads
  PER 1.1 % (83 "missed"). Both counter gaps (42 frames from slot 0, 41
  from slot 2) span exactly the hole, and the master received all 3,734 of
  SEC 1's transmissions. The 8 packets "from slot 1" in that file are
  stale records inside the hole; a slot-1 unit cannot receive slot 1. See
  Known limitations and card #25.
- **Card #23 is visible, in the form it predicts for syncing.** The first
  packet each secondary sends after lock arrives garbled at every receiver:
  frame counter 5, payload bytes 0/2/3 = `0xA1 · 0x05 0x00`, a received
  header's bytes. Everything after it is clean. Identical damage at every
  receiver puts it in the transmitter's buffer before it sends. That fits
  the receive pointer wrapping onto the staged TX payload while a secondary
  only listens during SYNCING. The master never syncs and is unaffected.
  With three units, each unit hears two packets per frame, so the
  steady-state wrap #23 predicts for four units did not occur.

### Intercom TDMA radio layer (M0-M2 implementation)

New product hosted in this tree: the L1/L2 radio layer for the sports-officials
intercom — a fixed 4-slot TDMA broadcast flood over the same SX1262 PHY
(915.0 MHz, SF5 / BW 500 kHz / CR 4-5, 44-byte fixed packets, 12-symbol
preamble, +22 dBm). Slot width 20 ms (one constant,
`TDMA_SLOT_DURATION_US`); frame = 4 slots = 80 ms. Bring-up ran at 50 ms;
the flip landed 2026-08-17, and its 30-minute two-unit acceptance soak passed
on 2026-08-18 (see "Slot width 50 ms → 20 ms" below).

- **L1** [`src/tdma/sx126x_cmd.c`](src/tdma/sx126x_cmd.c) — app-owned raw
  SX1262 opcode layer (datasheet-cited) with all BUSY gating in one choke
  point. The native Zephyr driver (L0) is used for init only; its DIO1
  callback (which did SPI on the system workqueue) is detached after
  `lora_config()` and replaced with the TDMA port's own
  (findings: [`docs/tdma_layering.md`](docs/tdma_layering.md)).
- **L2** [`src/tdma/`](src/tdma) — single radio thread (sole runtime SPI
  owner) polling slot-tick + DIO1 semaphores; hardware TIMER2 on HFXO
  (explicit `clock_control_on()` request, see hardware test findings below)
  via the counter API at 1 MHz with an accumulating absolute alarm target;
  RX slots run the chip in continuous RX, the slot boundary itself is the
  window's end; TX staging during RX-slot slack; drop-oldest RX msgq;
  telemetry counters. Secondary units acquire the master's slot-0 beacon
  (snap, then proportional step clamped to ±500 µs/frame) and go SYNCING →
  RUNNING after 3 beacons under 250 µs error. That transition is one-way:
  `tdma_start()` is the only entry into SYNCING, so a locked unit never
  re-enters acquisition and the lock threshold governs start-up alignment
  only.
- **App** (the UART-shell test variant, retired 2026-09-25) — Kconfig
  role/slot (`TDMA_ROLE_MASTER`/`TDMA_ROLE_SECONDARY`,
  `TDMA_SLOT_ID`), telemetry print every 5 s over UART, and `tdma` shell
  commands (`tx`, `rx [s]`, `start`, `stop`, `stats`) for M0 manual bring-up.
- Build: `west build -b nrf5340dk/nrf5340/cpuapp -p always -- -DCONFIG_APP_TDMA_TEST=y`
  (+ `-DCONFIG_TDMA_ROLE_SECONDARY=y` for the second unit).

### TDMA field console (2026-07-21)

First "test container": the bench diagnostics sequence above, packaged as an
on-device TFT applet for field soak testing —
[`src/tdma_console/main.c`](src/tdma_console/main.c). Reuses the telemetry
console's display toolkit ([`src/tdma_console/ui_widgets.c`](src/tdma_console/ui_widgets.c))
and touch calibration ([`src/tdma_console/touch_cal.c`](src/tdma_console/touch_cal.c));
the radio path is
exclusively the TDMA L1/L2 shim (the old console's native-driver modules —
`radio_cfg`/`payload`/`pingpong` — are not used). Display-only: no UART,
logging, or shell.

- **One image for both units**: role (MASTER/SECONDARY) is chosen at boot and
  locks at the first soak start (`tdma_init()` is once-only); reboot to
  change. Defaults to SECONDARY — two unconfigured units just listen instead
  of both beaconing into slot 0. (Role selection moved off HOME into the
  power-on sequence on 2026-07-31; see that section below.)
- **SOAK TEST** — pick 5 min / 30 min / 2 h / continuous; the engine runs the
  full TX/RX frame exchange while the screen shows live per-soak deltas:
  tx/stale, rx/crc/bad-hdr, phase error and ppm, RSSI/SNR, BUSY faults, and
  elapsed/limit. STOP (or the elapsed limit) parks the engine and freezes the
  stats until BACK; BACK alone never kills a running soak.
- **Frame-continuity loss stats**: the peer's `frame_ctr` advances once per
  frame, so per-slot counter gaps are missed frames and repeats are stale
  retransmits (`miss` / `dup` on the soak screen) — content-level loss the
  CRC counters cannot see, and the number that matters when evaluating
  tighter slot widths in the field.
- **TX power adjustable** (−9…+22 dBm, keypad entry): the one PHY parameter
  exposed in the UI. New engine API `tdma_set_tx_power()` latches the value
  from any thread; the radio thread issues `SetTxParams` right before the
  next `SetTx` (chip in FS — the datasheet-legal window), so it applies from
  the next packet without disturbing slot timing. Everything else stays a
  compile-time constant in [`src/tdma/tdma.h`](src/tdma/tdma.h).
- **TOUCH CAL** — the console's 5-point calibrate/verify flow (made a
  mandatory power-on step on 2026-07-31; see that section below).
- The soak screen is laid out for a future slot-width selector (the engine
  still pins `TDMA_SLOT_DURATION_US` at compile time; making it runtime is
  the planned next engine change for width evaluation).
- Build:
  `west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-tft --`
  `-DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.overlay`
  `-DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.conf`
  (the pair was `display.overlay` + `tdma_console.conf` until 2026-09-25;
  the SD stack was added to this build on 2026-07-31).

### Soak test harness (2026-07-30)

_Retired 2026-09-25 with the streamline to two unit types (see that entry above); kept as history._

New [`src/soak/`](src/soak) subtree (`CONFIG_APP_SOAK`): individually flashable
bench soak tests that stream machine-ingestible CSV over the DK's VCOM UART,
for quick automated ingestion of bench-top radio performance. Reuses the
[`src/tdma/`](src/tdma) L1/L2 engine unchanged — that layer has no dependency
on the application Kconfig symbols, only on the `TDMA_*` constants and the
`tdma_config` it is handed.

- **Role from a jumper, not a build flag.** P1.10 → 3V3 = MASTER (slot 0),
  open = SECONDARY (slot 1), via an internal pull-down
  ([`src/soak/role_select.overlay`](src/soak/role_select.overlay)). One image
  flashes to both units. P1.10 is free (the Wio-SX1262 shield uses only
  P1.00–P1.09) and sits on the port-1 header clear of the Arduino shield.
  Fails safe to SECONDARY if the GPIO cannot be read, so a unit never claims
  the timing-master role by accident.
- **Shared runner** [`src/soak/soak_run.c`](src/soak/soak_run.c) — brings up
  the engine at the requested TX power on the locked PHY, runs a fixed-length
  exchange, emits `SOAK,<id>,...` lines (meta / hdr / row / done / END) and a
  derived packet-error rate. The `SOAK,` prefix survives interleaved Zephyr
  log output.
- **First test** [`src/soak/s01_baseline_m9dbm_5min/`](src/soak/s01_baseline_m9dbm_5min)
  — both radios at −9 dBm (the SX1262 PA minimum, giving later tests a
  worst-case floor), 5 minutes, 5 s sample cadence. Each test owns a `main()`
  guarded by its own `CONFIG_SOAK_TEST_*` symbol, so only the selected one
  links; CMake globs `src/soak/**/*.c`, so adding a test needs no build-file
  edit.
- **Ingest** [`src/soak/tools/ingest_soak.py`](src/soak/tools/ingest_soak.py)
  — parses a saved capture or reads the serial port live, writes tidy CSV,
  prints the run metadata and summary.
- Build:
  `west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-soak-s01 --`
  `-DEXTRA_CONF_FILE=src/soak/s01_baseline_m9dbm_5min/s01.conf`
  `-DEXTRA_DTC_OVERLAY_FILE=src/soak/role_select.overlay`

### Soak logging to SD (2026-07-31)

Every soak on the field console now writes a binary record log to the microSD
card — [`src/tdma_console/soak_log.c`](src/tdma_console/soak_log.c) over
[`src/tdma_console/sd_log.c`](src/tdma_console/sd_log.c). Automatic: it hangs
off `soak_start()` / `soak_finish()`, the only ways in and out of a run, so no
soak goes unlogged. Each run opens the next free `/SD:/SOAKnnn.BIN`.

- **Fixed 64-byte binary records, not CSV.** The payload alone is 40 bytes,
  which as hex text is 80 characters before any telemetry, and formatting it
  per packet costs far more than a `memcpy`. 64 B also divides 512 exactly, so
  eight records fill a FAT sector with nothing wasted and no partial-sector
  rewrite. `BUILD_ASSERT`s pin both the size and the division. Text formatting
  happens offline in the decoder, where it is free.
- **Rate budget** — sized for the 20 ms production slot, which is now the
  build's actual width: a 4-unit 80 ms frame yields 3 RX + 1 TX = 50
  records/s = 3.2 KB/s = ~6 sector writes/s. On the 2-unit bench it is half
  that.
- **Producers never touch the card.** Records go into a 128-deep `k_msgq`
  ring; a dedicated preemptible writer thread drains it. Mount, open, write
  and close all happen there, so a card stall blocks only the writer — never
  the UI loop, and never the cooperative radio thread that outranks both.
  Every FatFs call being on one thread also makes the LFN BSS working buffer
  (documented as not thread-safe) safe by construction.
- **Logging failure is non-fatal** — a missing or unreadable card must not
  abort a radio test. The soak runs regardless and the soak screen shows
  `LOG FAIL <stage> <errno>`, where stage is `disk` (card never came up),
  `mount` (volume rejected) or `open` (file creation failed).
- **What is captured**: full payload per received packet plus `t_us` (DIO1
  edge, for jitter), `frame_ctr`, RSSI/SNR and sync state; one record per
  *transmitted* packet; and a 1 Hz engine-counter snapshot plus a baseline
  snapshot at t=0. Record 0 is a META record describing role, PHY, TX power
  and slot/frame timing, so a decoded file is self-contained.
- **`tdma_rx_msgq` deepened 8 → 32** (`TDMA_RX_MSGQ_DEPTH` in
  [`src/tdma/tdma.h`](src/tdma/tdma.h)). Depth must cover the consumer's worst
  stall, not the average rate: at 50 records/s a couple of hundred ms of
  consumer stall silently dropped packets *before* any logger could see them.
- **Decoder** [`src/tdma_console/tools/decode_soak_log.py`](src/tdma_console/tools/decode_soak_log.py)
  — binary to tidy CSV plus a summary (RSSI/SNR spread, frame continuity,
  PER, engine counters). Detects on-device ring drops from `seq` gaps. Payload
  integrity is checked against the firmware's `payload[i] = base + i` pattern,
  recovering `base` by majority vote across all 40 positions rather than
  trusting `payload[0]` — one corrupted first byte would otherwise make every
  byte look wrong.

> [!NOTE]
> Because packet CRC is on, the modem drops corrupted frames and they never
> reach the log, so payload checking reads clean essentially always. Measuring
> true BER would mean disabling the packet CRC so damaged frames still surface.

### First logged soak + log format v2 (2026-08-01)

First 5-minute two-unit soak captured to SD and decoded off the cards, at
−9 dBm on the 50 ms bench slot. The record format, writer and decoder were
validated end to end on the first attempt — no padding or byte-order
mismatch — and **neither file had a single `seq` discontinuity**, so the ring
dropped nothing at ~1.16 KB/s.

- **Zero packet loss in both directions**: 1486 frames in which both units
  logged a packet, 0 missed / 0 dup / 0 payload bit errors either way, one
  CRC error total. RSSI −67…−62 dBm, SNR 6…10 dB. The secondary's three
  "extra" frames (counters 2–4) are simply the master starting its soak three
  frames later, not loss — both files end on counter 1490.
- **Slot timing has enormous margin at the 20 ms target.** RX inter-arrival
  error against the nominal frame: σ = 21 µs, p50 ≈ 0, p99 +55 µs, full range
  −135…+69 µs; the secondary's sync phase error held to −63…+36 µs. A 20 ms
  slot leaves 12.37 ms of guard after the 7.632 ms time-on-air, so the worst
  observed excursion is ~1 % of the budget. Clock discipline is not what
  limits tightening the slot.
- Caveat: ~45 dB of margin over SF5/BW500 sensitivity on the bench. 0 % PER
  here says the stack is correct; it says nothing about range.

Three logging defects the first real capture exposed, all now fixed:

- **TX records counted submissions, not transmissions.** The master logged
  3693 TX records against 1499 actual transmissions — `tdma_tx_submit()`
  succeeds whenever the engine's stage buffer is free, which the 20 ms UI loop
  hits ~2.4× per frame, and every extra payload was overwritten in place
  before its slot. Two thirds of the file described bytes that never reached
  the air. The runner now holds a payload "armed" and logs it only once the
  engine's `tx_done` confirms it went out, so a TX record is a transmission
  and carries the payload that was actually sent.
- **`ppm` was destroyed by clamping.** v1 squeezed the clock-rate estimate
  into the record's `int8` `snr` field, where it railed at ±127 — and that is
  the single measurement that matters most for tightening the slot. The phase
  error had the same latent bug (`int16` cannot hold the ±frame/2 range).
  Format **v2** gives STATS a typed 40-byte overlay (`struct soak_stats`) with
  both as `int32`, making room by dropping `rx_ok`/`missed`/`dup` — which the
  decoder derives from frame-counter continuity anyway.
- **Engine counters are absolute, not per-soak.** `tdma_start()` deliberately
  does not reset `eng.telem`, so a second soak in one boot keeps counting: the
  master's log read `tx_done=1608` absolute against 1499 for that run. A
  baseline STATS record is now written at t=0 and the decoder reports
  `engine (this run)` alongside `engine (absolute)`.

The decoder reads both v1 and v2, so the captures above remain readable; it
prints the format version and warns when a v1 log's `ppm` sits on the int8
rails.

### Log naming, drawers + SD file browser (2026-08-01)

Soak log names now encode the run: `<PWR>_<DUR>_NNN.BIN` — `M`/`P` for the
power sign and minutes/hours for the duration (`M9_5M_000.BIN`,
`P22_2H_007.BIN`, `CT` = continuous). Long-name support is always compiled in
(exFAT selects `FS_FATFS_LFN`, and LFN works on FAT32 too), so the old 8.3
constraint no longer applies. Logs can be pointed at one of four fixed
top-level drawers (`DRAWER0`–`3`) or the root, selected on the soak-pick
screen ("LOG" row); a missing drawer is created when a log opens there.

- **FILES browser** on HOME: list (directories first, 10 rows, DK-button or
  tap scroll), **delete** with a two-press confirm, **move** between root and
  drawers (`fs_rename`, destination auto-created), **+ DRAWER** (next free of
  the four), and an optional **label** appended before the extension
  (`M9_5M_000_RANGE1.BIN`) — never part of a default name.
- **Alpha keypad**: third key table for the existing modal (A–Z 0–9 `_`,
  6×7 grid; the grid's column count is now per-table). Used for labels.
- **All card I/O stays on the writer thread**: the browser submits one op at
  a time (`sd_fsop_*` in soak_log.c) and polls from the 20 ms loop — the LFN
  working buffer is a shared static (not thread-safe) and a sick card must
  never stall the UI, so this holds by construction, browser included.
- Foreign directories (e.g. Windows' "System Volume Information") list but
  are not enterable; >64 entries shows a truncation marker.

### TX-start latency instrumentation (2026-08-12)

Instrumentation to measure `TDMA_TX_START_LATENCY_US` on the bench — no
constant or behaviour change yet. The header defines the constant as the
chip-only SetTx→first-preamble latency (100 µs), but its one consumer,
`tdma_core_sync_feed()`, needs the full boundary→first-preamble delay, which
additionally contains the radio-thread wake and the `slot_tx_enter` SPI
sequence (ClearIrqStatus / SetFS / SetTx, each behind BUSY waits) — the
~1 ms gap. The plan is to redefine the constant as boundary→air and set it
from measurement rather than refine the chip figure.

- **Two telemetry fields** (additive; the STATS v2 SD overlay is at exactly
  40 bytes and is deliberately untouched): `dt_by_slot[]` — `last_evt_dt_us`
  attributed to the slot it occurred in — and `tx_evt_dt_us`, that dt latched
  only on TxDone events, i.e. this unit's own boundary→TxDone delay
  (TX latency + time-on-air), independent of sync alignment.
- **Soak screen gains a `dtx N  drx N` row**: own TX dt plus the watched peer
  slot's RX dt (2-unit kit: master watches slot 1, secondary slot 0).
- **Why both**: the open "~1.1 ms per-role asymmetry" (07-21 findings, below)
  may not be per-role at all — `slot_tx_enter` has no role branch. A locked
  secondary aligns its boundary through the wrong constant and so sits late
  by exactly the constant's error, which makes *cross-unit*
  boundary-referenced readings differ by ~1.1 ms even if the two units' true
  TX latencies are identical. Own `dtx` is immune to that offset; cross-slot
  `drx` carries it. Comparing the two separates a real asymmetry (split
  per-role constants) from a plain constant error (one corrected constant).
- Built-in self-check: the secondary's `drx` should read ≈7732 µs
  (TOA_const + L_const) regardless of the true latency — the sync loop
  aligns it there by construction. If it doesn't, the instrumentation itself
  is suspect.
- These fields join the bench-diagnostics strip candidates (see
  "Bench diagnostics" below), except `tx_evt_dt_us` is likely worth keeping
  as the permanent regression check on the constant.

### TX-start latency measured; TOA + latency constants corrected (2026-08-16)

Bench measurement over the instrumentation above (both consoles, 50 ms
slots) closed the question: **there is no per-role asymmetry**. Both units
read boundary→TxDone `dtx` = 8292 µs — identical — and every self-check
landed exactly: the secondary's `drx` held at 7732 µs (TOA_const + L_const,
where the sync loop parks it by construction) and the master's
`drx` = 8852 µs matched the predicted sync offset Δ = 560 µs to the
microsecond.

The "~1.1 ms per-role asymmetry" from the 07-21 findings is thereby
resolved as a **constant error propagated through sync alignment**: the
secondary aligned its boundary through the wrong constants and sat 560 µs
late (432 µs latency error + 128 µs TOA error), which any cross-unit
boundary-referenced reading picked up in full — while the TX paths
themselves are exactly role-symmetric.

Two constants corrected in [`src/tdma/tdma.h`](src/tdma/tdma.h); no code
change:

- **`TDMA_TOA_US` 7632 → 7760.** The derivation used SF7+'s 4.25-symbol
  preamble sync overhead; SF5/SF6 carry 6.25 symbols, so
  t_preamble = (12 + 6.25) × 64 = 1168 µs.
- **`TDMA_TX_START_LATENCY_US` 100 → 532**, redefined from the chip-only
  SetTx→air figure to the full boundary→air delay its one consumer — the
  sync equation — actually needs: radio-thread wake + the `slot_tx_enter`
  SPI sequence + chip latency. 532 = measured 8292 − TOA 7760.
  `tx_evt_dt_us` reading TOA + this constant is the standing regression
  check.

Effect: a locked secondary's boundary moves 560 µs earlier, coinciding with
the master's. Guard budget at the 20 ms target: worst in-slot completion is
532 + 7760 ≈ 8.3 ms, leaving ~11.7 ms of slack — now a measured number
rather than an estimate. Confirmed on the bench after reflashing both
units: all four soak-screen readings (`dtx`/`drx`, both roles) collapsed
to ≈8292 µs with tens-of-µs jitter, i.e. the sync offset Δ went to zero
as predicted.

### Payload staging moved off the UI loop (2026-08-19)

First bench run at 20 ms exposed an application-layer regression the flip
caused but did not itself contain: the master reported `stale_retx` = 79
against 224 transmissions — **35 % of packets re-sending the previous
payload** — growing linearly from the first second. The 50 ms baseline was
48 in 30 minutes. Confirmed independently from the air: 30.4 % of
consecutive RX packets carried an identical payload base, i.e. both units
were doing it.

**The engine was never at risk, and that distinction is the point.** Through
the whole 35 % run `dtx` held 8249–8300 µs and `slot_timeouts` stayed 0 —
the radio thread is cooperative at `K_PRIO_COOP(4)` and preempts the UI, so
slot timing was untouched. What starved was the *application data path*,
which shared a thread with the screen.

**The budget the flip actually cut.** The engine takes a staged payload in
`rx_slot_slack_work()`, which runs at every RX-slot entry, so the last
chance before a unit's next TX comes `TDMA_SLOT_COUNT - 1` slots after its
own. The window runs from TxDone to that entry:

    (SLOT_COUNT - 1) × SLOT_DURATION - (TX_START_LATENCY + TOA)

141.7 ms at 50 ms slots, **51.7 ms at 20 ms** — a 63 % cut, and
role-symmetric. `draw_soak_dynamic()` clears the body with a full-width SPI
fill before drawing its rows and overruns 51.7 ms on its own, so every
repaint dropped a frame's payload: 4 repaints/s against 4.4 stale/s. The
same cost fitted inside the 50 ms budget, which is why it only surfaced
after the flip.

Fix, in [`src/tdma_console/main.c`](src/tdma_console/main.c):

- **New `soak_data_tid` at `K_PRIO_COOP(6)`** — below the radio thread,
  above every preemptible thread. It owns the RX drain, the TX arm/re-stage
  and the periodic STATS snapshot, ticking at 2 ms. It never touches SPI and
  never blocks, so sitting above the UI costs the UI nothing. The UI loop
  keeps drawing, input, and the teardown only: `soak_finish()` closes the log,
  which waits on the card writer for up to 5 s and must not run on a
  cooperative thread. The data thread therefore latches `soak.expired` and
  the UI performs the stop.
- **A headroom gauge on the soak screen**, `lat N/51ms`, since the question
  is ongoing rather than one-off: worst pass-to-pass latency of the data
  thread against the budget. Under budget, staging cannot be late and
  `stale_retx` is impossible; it degrades visibly long before any packet
  does, which is what makes it useful as the product takes on more concurrent
  work.
- Cost: +448 B flash, +1712 B RAM.

> [!NOTE]
> The gauge first shipped measuring the interval between successful
> `tdma_tx_submit()` calls, which is wrong: a submit can only follow the
> previous *transmission* (the `tx_armed` gate that makes the log record
> transmissions rather than submissions), so that interval has a floor of one
> frame period — 80 ms — and cannot be compared with a within-frame deadline.
> It read 88 ms on a healthy master (80 ms frame + 8 ms) and 349 ms on a
> secondary (acquisition: it locked at +0.676 s and cannot transmit before
> RUNNING). Both were correct behaviour reported as failures. The budget
> constrains how long after TxDone the application takes to re-stage, which is
> bounded by how promptly the data thread runs — hence pass-to-pass latency.

**Result** — 20 ms, +0 dBm, two units, ~29 s, secondary logged to SD:
`stale_retx` **0 across 353 transmissions**, flat from the first second.

### Slot width 50 ms → 20 ms (2026-08-17)

`TDMA_SLOT_DURATION_US` 50000 → 20000 in [`src/tdma/tdma.h`](src/tdma/tdma.h)
— step 4, the last step of the tightening sequence. Frame follows to 80 ms
and `TDMA_SLOT_ACTIVE_US` to 18 ms, both derived. A straight constant flip:
no runtime slot-width selector, which stays a separate card.

**Acceptance soak: passed** (Trello card #6, 2026-08-18). The run was
30 min, both bench units at +0 dBm, SD-logged on both sides (master
`P0_30M_000.BIN`, secondary `s_P0_30M_000.BIN`, both `META slot=20000us`).
The logs are not kept in the repo; the figures below were decoded from them.
It ran with the payload-staging fix below, which is committed as `3746f11`
at 01:05 on 2026-08-19, so the session evidently ran past midnight. The
proof is `stale_retx`, which is 0 throughout. Results against the pass
criteria at the end of this entry, from the secondary's log unless noted
(STATS once a second, 1740 samples after the first minute):

| Signal | Bar | Measured |
|---|---|---|
| Lock | single-digit seconds | **1.66 s** |
| `dtx` / `drx` | 8292 ± tens of µs, same value both roles | `evt_dt` median **8292** (8231–8355) secondary, **8288** master |
| `phase_err` | median ≈ 0, p95 \|err\| ≤ 50 µs, worst < 125 µs | **+1 / 38 / 75 µs**, σ 17; no drift (half-run means +0.06 / +0.07) |
| Link, master → secondary | PER 0.000 %, 0 missed/dup, 0 `seq` gaps | **22,483 received, 0 missed / 0 dup, 0 `seq` gaps** |
| Link, secondary → master | same | master `rx_done` **22,480** = secondary `tx_done` 22,480; log: see below |
| Counters | `stale_retx`, `busy_timeouts`, `slot_timeouts` all 0 | **all 0, both units** (22,500 / 22,480 transmissions) |
| Frame spacing | 80 ms | RX timestamps 80,000 ± 23 µs (σ), both units |
| `ppm` | mean within a few ppm of −6.9 | mean −1.7, standard error ≈ 6, so consistent; σ 265, see below |

`rx_crc_err` totalled 1 (master) + 3 (secondary). Two things stand out:

- **The master's log has a hole: the known one, again.** Records
  29,344–29,472 start on a sector boundary and hold 112 zeroed records plus
  stale sectors from an earlier run. Decoded naively, the master's file
  reads PER 26 % (7,889 "missed"), because `frame_ctr` jumps into the stale
  data and back. Outside the hole, `seq` and `frame_ctr` are continuous, and
  the engine counters above show the link lost nothing. This is the second
  occurrence of the 8 KB hole under Known limitations, now at 20 ms, and is
  tracked as card #25.
- **`ppm` σ is 265, not the ~170 predicted.** It matches the 304 seen in
  the short runs below, so the spread is real, not noise. Card #24 has the
  explanation: at this frame rate the per-beacon figure is phase jitter
  ×12.5, not a rate estimate. The mean is still the signal.

The short bench runs at 20 ms that preceded the soak (~29 s, +0 dBm, two
units, secondary logged to SD; sample counts in parentheses) had cleared
every criterion they were long enough to test:

| Signal | Bar | Measured |
|---|---|---|
| Width on air | 80 ms frames | 12.46 frames/s ✓ |
| `dtx` | 8292 ± tens of µs | median **8292**, 8250–8318 (29) |
| `phase_err` | median ≈ 0, p95 ≤ 50 µs, worst < 125 µs | **+1 / 38 / 38 µs** (29) |
| Lock | single-digit seconds | **0.68 s** |
| Link | PER 0 %, 0 missed/dup, 0 `seq` gaps | all clean (326 pkts) |
| Counters | `stale_retx`/`slot_timeouts`/`busy_timeouts` = 0 | all 0 |
| `ppm` | mean within a few ppm of −6.9 | **inconclusive** — see below |

`dtx` landing on 8292 is the standing regression check on the corrected
`TDMA_TOA_US` 7760 and `TDMA_TX_START_LATENCY_US` 532: it holds at the new
width. The phase envelope (−33…+38 µs) is *tighter* than either 50 ms
baseline, the direction the 2.5× beacon rate predicted.

Two reasons those runs could not stand in for the acceptance soak:

- **29 samples over 29 s**, against 575 over 11 min for the 50 ms baseline.
  A short run sees a narrower envelope purely from fewer draws, so the phase
  figures will widen in a real soak. They pass with room; that is not proof.
- **`ppm` cannot be tested at this length.** Mean +32.3, median −18, σ 304
  over 24 samples puts the standard error near 62 ppm — consistent with the
  true −6.9 and with much else besides. Worth noting σ came in at 304 against
  the ~170 predicted for 80 ms frames; with 24 samples that is likely real
  rather than noise, so it is a thing to watch in the 30-minute run rather
  than a settled figure.

Pre-flight audit, all clear at head:

- Every variant's `tdma_config.slot_duration_us` passes the macro, never a
  literal — [`soak_run.c`](src/soak/soak_run.c),
  [`tdma_app/main.c`](src/tdma_app/main.c),
  [`tdma_console/main.c`](src/tdma_console/main.c) — and `tdma_init()`
  rejects a mismatch, so a stale literal would fail loudly rather than run
  a split-brain frame.
- Worst in-slot completion is `TDMA_TX_START_LATENCY_US` + `TDMA_TOA_US` =
  8292 µs against an 18 ms `SLOT_ACTIVE`, ~9.7 ms of headroom. That governs
  the TX timeout only; RX is continuous, so the slot boundary itself ends
  the window.
- `TDMA_SYNC_STEP_CLAMP_US` stays 500 µs, now per 80 ms frame — max slew
  rises 2.5 → 6.25 ms/s. Intended: beacons arrive 2.5× more often, so the
  loop should correct 2.5× faster.
- `TDMA_RX_MSGQ_DEPTH` 32 was already sized for the 20 ms 4-unit case.
- The HFXO request in [`tdma_port.c`](src/tdma/tdma_port.c) becomes
  load-bearing rather than prudent: the ~−1000 ppm of RC drift it avoids is
  absorbable at 50 ms, not at 20 ms. Comment updated to say so.

**Pass criteria for the acceptance soak** (30 min, both bench units, +0 dBm,
SD-logged both sides; 50 ms baselines in parentheses):

| Signal | Bar | Baseline |
|---|---|---|
| Lock | SYNCING → RUNNING in single-digit seconds | 2.30 s |
| `dtx` | 8292 ± tens of µs, both roles | 8292 |
| `drx` | collapses to the same value both roles | 8290 / 8291 |
| `phase_err` | median ≈ 0, p95 \|err\| ≤ 50 µs, worst < 125 µs | 0 / 24 / 51 |
| Link | PER 0.000%, 0 missed/dup, 0 `seq` gaps | same |
| Counters | `stale_retx`, `busy_timeouts`, `slot_timeouts` all 0 | same |
| `ppm` | mean within a few ppm of −6.9 | −6.9 |

Two readings that would be misread as regressions: per-sample `ppm` σ should
*grow* to ~170 (the estimator divides the same ±13 µs timestamp jitter by an
80 ms frame instead of 200 ms — the mean is the signal), and `phase_err`
growth toward the 250 µs bar is a red flag even if the unit locks, since
2.5× more frequent beacons should make the envelope equal or tighter.

**Soak duration constraint at 20 ms**: `frame_ctr` wraps at 1.46 h (see
Known limitations). Runs needing cross-file pairing must stay under that;
the console's 2-hour preset exceeds it.

### Sync lock threshold tightened to 250 µs (2026-08-16)

`TDMA_SYNC_LOCK_ERR_US` 1000 → 250 in [`src/tdma/tdma.h`](src/tdma/tdma.h).
Step 3 (second half) of the 50 ms → 20 ms tightening sequence, taken after
the accumulate change below soaked clean.

**What the constant actually governs.** Acquisition only. It is the bar
`SYNCING` measures against to reach `RUNNING`, and there is no
`RUNNING` → `SYNCING` path — `tdma_start()` is the sole entry into
`SYNCING`, so once a unit locks it stays locked and a beacon over the
threshold only resets `lock_streak`. Tightening therefore cannot destabilise
a running link; it raises how well aligned a unit must be before it starts
transmitting, at the cost of possibly sitting in `SYNCING` a little longer.

**Measured basis** — 11 min two-unit soak, 50 ms slots, +0 dBm, both roles
logged to SD (fmt v2):

- Secondary steady state (575 samples after 60 s): `phase_err` median 0 µs,
  mean −1.7 µs, p95 |err| 24 µs, envelope −51…+27 µs. No drift — first-half
  mean −1.85 µs vs second-half −1.61 µs.
- Worst |`phase_err`| anywhere in the run was 51 µs, so 250 µs keeps ~5×
  margin over the observed envelope.
- Acquisition was already inside ±11 µs before the first lock (`SYNCING` →
  `RUNNING` at +2.30 s), so the tighter bar would not have delayed this
  run's lock at all.
- `drx` median 8290 µs (secondary) / 8291 µs (master), σ 13–15 µs — both
  roles agreeing to 1 µs, as the constants correction above predicted.
- Link: PER 0.000%, 0 missed, 0 dup over 3322 packets; `slot_timeouts`,
  `stale_retx`, `busy_timeouts` all 0 on the secondary.
- `ppm` is a real reading for the first time (v1 railed it at ±127): mean
  −6.9 ppm crystal-to-crystal offset. Its σ of 68 is measurement noise, not
  clock instability — a per-beacon estimate over one 200 ms frame turns the
  observed ±13 µs timestamp jitter into ±65 ppm.

### Phase corrections accumulate (2026-08-16)

`tdma_port_add_phase_adj()` in
[`src/tdma/tdma_port.c`](src/tdma/tdma_port.c) was named "add" but
implemented *set* (`atomic_set`), so a second post landing before the next
slot boundary silently discarded the first. Now `atomic_add`: corrections
accumulate into one pending value that `slot_alarm_cb()` folds in once via
`atomic_clear`.

No behavior change for today's call pattern — `tdma_core_sync_feed()` posts
one correction per beacon (one per frame) and boundaries consume four times
per frame, so there is never more than one outstanding value. It matters
once two posts can land between consumptions: a slew-limited loop splitting
a large error into partial steps, or a drift/ppm feed-forward writer
alongside the beacon correction, both on the table for the M3 filter work.
A dropped ms-scale correction leaves the boundary transiently misaligned
until the next beacon pulls it back — at 20 ms slots that is a meaningful
fraction of the guard budget, though it cannot change sync state (nothing
leaves RUNNING once locked).

- The race with the consumer is benign under add: a post either lands
  before the ISR's `atomic_clear` (applied at that boundary) or after
  (pending for the next). Nothing is lost.
- Negative adjustments still work through two's-complement wraparound on
  the unsigned add at the consumer; the `(uint32_t)` cast stays.
- **No consumption-side per-boundary clamp.** Post-side clamping already
  exists where it matters (steady-state step clamped to
  ±`TDMA_SYNC_STEP_CLAMP_US` at the call site), and the acquisition snap is
  deliberately unclamped and forward-only. A per-boundary application clamp
  belongs with the M3 sync-filter rework (`TODO(M3+)` in
  `tdma_core_sync_feed()`).

Step 3 (first half) of the 50 ms → 20 ms tightening sequence; tightening
`TDMA_SYNC_LOCK_ERR_US` to ~250 µs follows as a separate change once this
soaks clean. Task write-up:
[`docs/phase_adj_accumulate.md`](docs/phase_adj_accumulate.md).

### Mandatory power-on sequence + minimal HOME (2026-07-31)

The field console now runs a two-step, session-only power-on sequence before
HOME is reachable: **TOUCH CAL → SELECT ROLE**. Neither is reachable from HOME
afterwards — reboot to redo either.

- **Calibration first, deliberately**: the transform is RAM-only, so every
  boot starts from the identity mapping and the role could not otherwise be
  picked by touch. Point collection uses the *raw* samples, so it bootstraps
  correctly from the identity transform.
- **ACCEPT / REDO touch buttons** on the verify screen, so confirming no
  longer needs a DK button. These exist only in the verify phase, and that is
  the point: it is the one phase where a freshly solved transform is already
  active, so a tap lands where it looks — tapping ACCEPT accurately *is* the
  proof the fit is good. In collect (identity transform) and fail (solve
  rejected) an on-screen button would be unpressable, so the DK buttons stay
  the only control there, and remain a working fallback throughout.
- **Calibration text no longer collides with the first target.** The
  instruction line sat at `MARG + body + 2` and ran straight through the
  top-left crosshair — the one target you look at while reading it. It now
  derives its baseline from the same 15 % inset `cal_target()` uses, so it
  clears every target and stays correct if the panel geometry changes.
- **SELECT ROLE** — two full-width touch buttons. The highlight starts on the
  current role (SECONDARY by default), so confirming without moving takes the
  fail-safe option instead of creating a second master.
- **HOME reduced to SOAK TEST + TX pwr.** Role and touch cal left the menu;
  role moved into the HOME header (`FIELD TEST: MASTER`), since with two
  identical DKs on the bench it is the one thing that cannot be inferred by
  looking at them.

### Hardware test findings (2026-07-31) — SD card bring-up

Found while bringing the soak logger up against a 128 GB exFAT card; the
smaller FAT32 card had masked all of it.

- **Root cause 1 — the logger could reformat the operator's card.**
  `CONFIG_FS_FATFS_MOUNT_MKFS` is enabled by default, and the mount struct
  left `.flags` at 0. Zephyr's answer to a `FR_NO_FILESYSTEM` mount is then
  `f_mkfs(FM_ANY | FM_SFD)` — which reformats the card and, with `FM_SFD`,
  writes no partition table at all. A card that had been in the unit came back
  to a PC as garbage, consistent with a partial run of exactly that. Fixed
  three ways: `FS_MOUNT_FLAG_NO_FORMAT` on the mount, `MOUNT_MKFS=n` to remove
  the code path, and both documented at the site.
- **Root cause 2 — card I/O ran on the UI thread.** Mount, open and the
  session-file scan all happened inside `soak_start()`, and the scan only
  broke out of its 1000-candidate loop on `-ENOENT` — so a volume erroring
  every `fs_stat` produced a thousand SPI round-trips and a frozen UI. The
  scan now aborts on any non-`-ENOENT` error, and all card I/O moved to the
  writer thread. The UI can no longer be stalled by the card in any state.
  This also retired a ~1.5 s freeze on every soak start with no card inserted
  (`CONFIG_SD_INIT_TIMEOUT`).
- **Root cause 3 — the card never enumerated.** With the stage-aware error
  reporting added above, the failure reported `disk -134` (`ENOTSUP`) and
  `disk -116` (`ETIMEDOUT`) on successive attempts — meaning the filesystem
  was never reached, and exFAT support was never the issue. The *alternation*
  was the diagnostic: a genuinely unsupported card fails identically every
  time, so two different errors on identical attempts meant garbled responses,
  i.e. a physical-layer problem. Fixes, in the display overlay:
  `power-delay-ms` 1 → 250 (the binding's own note says to raise it when card
  init misbehaves; a large SDXC card draws far more inrush than the smaller
  one and the SD spec allows 250 ms before a card must answer), and
  `spi-max-frequency` 24 MHz → 4 MHz (the logger needs ~3.2 KB/s, so this
  keeps two orders of magnitude of headroom while being far kinder to a card
  on jumper wires sharing the display bus). `sd_log_mount()` also retries the
  bring-up three times, 250 ms apart. Verified writing reliably afterwards.
- **exFAT support added** for cards Windows will not format as FAT32:
  `CONFIG_FS_FATFS_EXFAT=y` (which auto-selects `FS_FATFS_LFN` — FatFs
  requires it) plus `CONFIG_FS_FATFS_LBA64=y` for GPT-partitioned cards.
  Filenames stay 8.3-clean so the same naming works on FAT32.

### Hardware test findings (2026-07-21) — link closed full-duplex

Supersedes the same-day entry below on two points (the "benign" timeout ratio
and the guard-lead fix): both were wrong, found by adding per-slot bench
diagnostics (next section) and reading the chip's actual state instead of
inferring it from DIO1 activity alone.

- **Root cause 1 — dead alternating RX windows.** The per-slot `SetRx`
  timeout left every other RX window non-functional: armed and confirmed in
  RX mode by `GetStatus`, but never raising a DIO1 edge. `arm/slot` vs
  `evt/slot` tallies made this unambiguous (e.g. `[119 119 119 118]` armed
  against `[21 118 1 117]` events), and the raw DIO1 edge counter matched the
  accounted events exactly, ruling out lost interrupts. This — not a "benign
  2:1 ratio" — was the earlier master-side timeout:TX anomaly and the root
  cause of the secondary's marginal (~1/min) beacon reception. Fix: RX slots
  now run the chip in continuous RX (`SX126X_RX_CONTINUOUS`); the slot
  boundary is already the window's end, so the chip's own timeout was
  redundant. `slot_timeouts` no longer fires for RX slots as a result.
- **Root cause 2 — HFCLK on the uncalibrated internal RC oscillator.**
  Nothing in this build requests HFXO (no BLE/802.15.4 stack does it
  implicitly), so TIMER2 — the slot clock — free-ran on the internal RC.
  Measured ~1000 ppm relative drift between the two units from consecutive
  beacon timestamps, ~25–40x a crystal's tolerance and enough on its own to
  keep the sync loop from converging. Fix: `tdma_port_init()` now issues an
  explicit `clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF)`, held for the
  engine's life. Dropped the secondary's steady-state `phase_err` from
  ~400 µs to single-digit µs and `ppm` to ~0.
- **Root cause 3 — the previous guard-lead fix was itself a bug.**
  `TDMA_RX_GUARD_LEAD_US` biased the *entire* frame 1.5 ms early, not just
  the secondary's RX window, so its own TX slot fired 1.5 ms before the
  master was listening and the master decoded none of it. This was masked
  until root cause 2 was fixed — the old, sloppy ~400 µs phase error
  happened to leave just enough of the 1040 µs preamble inside the master's
  window to pass by luck. Removed outright: continuous RX (root cause 1)
  makes a guard lead unnecessary.
- **Result**: full duplex on hardware, ~5 packets/s each direction, zero
  CRC/header errors either side, sync lock in ~6 s, `phase_err` settling to
  single-digit µs, RSSI −36…−40 dBm / SNR +7…+9 dB throughout.
- **Open items before tightening to 20 ms slots**: TX-start latency
  appeared to differ between master and secondary by ~1.1 ms — resolved
  2026-08-16 as a constant error propagated through sync alignment, not a
  per-role difference: measured role-identical, and both
  `TDMA_TX_START_LATENCY_US` and `TDMA_TOA_US` corrected (see "TX-start
  latency measured" above); `tdma_port_add_phase_adj()` overwrote rather
  than accumulated a pending correction (latent — only one correction is
  issued per beacon today) — resolved 2026-08-16, it now accumulates (see
  "Phase corrections accumulate" above). Both are now closed, and the
  tightening sequence completed 2026-08-17 with the slot flip itself
  (step 3: `TDMA_SYNC_LOCK_ERR_US` 1000 → 250 µs; step 4:
  `TDMA_SLOT_DURATION_US` 50 → 20 ms). What remains before calling 20 ms
  proven: the 2-unit acceptance soak at the new width, then a 4-unit soak
  (gated on building units 3 and 4), then the field campaign.

### Bench diagnostics (2026-07-21)

Added to `tdma_get_telemetry()` / printed by `tdma stats` and the 5 s
periodic print, to tell apart "receiver never armed" from "armed but heard
nothing" from "heard energy but never completed a packet" — the ambiguity
that made root causes 1–3 above hard to see from the pre-existing counters
alone:

- `rx_arm` / `arm_by_slot[]` — `SetRx` issuances, per slot.
- `dio1_edges` — raw DIO1 ISR edge count, to catch events dropped between the
  pin and the drain (none were; see root cause 1).
- `drain_empty` — drains that read `IrqStatus == 0`.
- `preamble_det` / `header_valid` — `PreambleDetected` / `HeaderValid`
  latched in `IrqStatus` but deliberately *not* routed to DIO1
  (`SetDioIrqParams` takes the enable mask and the pin routing separately),
  so a live receiver hearing energy can be told apart from a dead one even
  when no full packet ever completes.
- `rx_mode_bad` / `last_chip_mode` — post-`SetRx` `GetStatus` mode check.
- `last_ppm` — relative clock-rate estimate between local and master slot
  clocks from two consecutive beacon timestamps and the frame counter delta
  in the header, free of the ±frame/2 wrap that makes `last_phase_err_us`
  ambiguous over long gaps between beacons.
- `evt_by_slot[]` / `last_evt_dt_us` — per-slot event counts and
  boundary-to-event delay, isolating which slot a symptom is in.

Bring-up scaffolding, not part of the M2 design — strip once the link is
proven at the 20 ms production slot width.

### SDK setup fixes (Windows / NCS v3.2.0)

- New [`patches/0003-lora-h-native-driver-api-compat.patch`](patches/README.md):
  stock NCS v3.2.x ships an older `include/zephyr/drivers/lora.h` missing
  `SF_5`, the narrow `BW_*` members, `packet_crc_disable` and the
  `recv_duty_cycle_async`/`airtime` API — nothing in this repo compiled
  against it. The patch mirrors the newer upstream header the vendored driver
  was written against.
- `patches/apply.sh` now applies with `--ignore-whitespace` (CRLF checkouts on
  Windows put CRLF driver files into the SDK; the LF patches otherwise refuse
  to match) and globs all numbered patches.

### HOME menu

_Retired 2026-09-25 with the streamline to two unit types (see that entry above); kept as history._

The console boots to a HOME screen. Navigate with the DK buttons
(**B1 = UP, B2 = DOWN, B3 = OK, B4 = BACK**) or by touch. The footer shows the
live applied-config summary.

- **SEND ONCE** — Fire a single transmit. Snapshots the current PAYLOAD bytes and
  the APPLYed RF config, signals the background TX thread to call `lora_send()`,
  and switches to the **TX STATUS** sub-screen: a spinner while in flight, then
  the result (SENT/FAILED, driver return code, byte count, and the config used).
  Requires a prior successful APPLY and a non-empty payload.
- **PING PONG** — Symmetric two-radio link test (both units run identical
  firmware; there are no roles). Enters continuous RX on the applied config;
  **SEND PING** transmits a 44-byte ping and the peer echoes a pong with the same
  sequence number, so the originator can measure round-trip time. A **STATS**
  sub-screen shows sent/received/lost counts and RTT last/min/max/avg;
  **RESET STATS** clears them. Outstanding pings time out after 1 s. All state is
  shown on the STATS sub-screen (this build has no serial output).
- **CONFIG** — Stage-then-apply RF configuration. Rows for frequency, spreading
  factor, bandwidth, coding rate, TX power, preamble, CRC, IQ, and network sync
  edit an in-RAM _shadow_ `lora_modem_config`; a `*` in the header marks it dirty.
  **APPLY** validates the shadow (902–928 MHz, −9…+22 dBm, preamble ≥ 6 and ≥ 12
  for SF5/SF6) and commits it with a single `lora_config()` call. Enum/bool rows
  cycle in place; numeric rows open the keypad.
- **PAYLOAD** — TX payload editor. Displays the current bytes as hex with a length
  counter (up to 255 bytes). **EDIT** opens the hex keypad; **CLEAR** empties it;
  presets load `0x00…`, `0xFF…`, an incrementing ramp, or the ASCII test vector
  `BLUEFROG`.
- **TOUCH CAL** — Touchscreen calibration. Tap five on-screen crosshairs; the
  firmware least-squares-fits a 6-parameter affine transform (correcting the
  rotated / offset / swapped XPT2046 axes versus the display) and offers a verify
  step that shows where taps land. The fit is held in RAM (re-run after a
  reflash). Fully operable with the DK buttons, so it works even before touch is
  calibrated.

Shared modal sub-screens: **KEYPAD** (reusable HEX entry for payloads, DEC entry
— with `.` and `+/-` — for numeric config fields) and the read-only **TX STATUS**
and **PING PONG STATS** views.

### Added (modules)

_The telemetry console's modules were retired 2026-09-25; `ui_widgets` and `touch_cal` live on in `src/tdma_console/` as the TFT unit's toolkit._

- **Staged RF config** — [`src/radio_cfg.c`](src/radio_cfg.c) /
  [`.h`](src/radio_cfg.h). Shadow + applied `lora_modem_config`, clamping
  setters/steppers, `radio_cfg_validate()` (datasheet limits), `radio_cfg_apply()`
  (the single commit point), dirty tracking, and value/summary formatters.
- **Payload buffer** — [`src/payload.c`](src/payload.c) /
  [`.h`](src/payload.h). Byte buffer + length, canned presets, hex format/parse.
- **UI toolkit + keypad** — [`src/tdma_console/ui_widgets.c`](src/tdma_console/ui_widgets.c) /
  [`.h`](src/tdma_console/ui_widgets.h). RGB565 blit primitives (CFB glyph data only — see the
  CFB note in the README), hit-testing, button and config value-row widgets, and
  the reusable HEX/DEC keypad modal.
- **Touch calibration** — [`src/tdma_console/touch_cal.c`](src/tdma_console/touch_cal.c) /
  [`.h`](src/tdma_console/touch_cal.h). In-RAM affine transform + a 5-point least-squares
  solver (mean-centred, single-precision, FPU-optional).
- **Ping/pong link test** — [`src/pingpong.c`](src/pingpong.c) /
  [`.h`](src/pingpong.h). A single dedicated RX thread that owns the radio while
  active (polls `lora_recv()` in short slices, sends ping/pong via `lora_send()`),
  mutex-guarded stats, and the 44-byte packet format.
- **Screen router** — [`src/console.c`](src/console.c). Main loop + screen state
  machine (HOME, CONFIG, PAYLOAD, KEYPAD, TX STATUS, CALIBRATE, PING PONG, PING
  PONG STATS), debounced touch capture, and single-owner drawing (only the main
  loop calls `display_write()`).

### Test functions & modes

_Retired 2026-09-25 with the streamline to two unit types (see that entry above); kept as history._

- **One-shot TX + radio status dump** — [`src/main.c`](src/main.c) (LoRa-send
  variant). Transmits a 16-byte payload once and dumps the SX1262 IRQ status and
  latched device errors over raw SPI (`dump_radio_status()`). Use for radio-only
  bring-up without the display stack.
- **SEND ONCE** — single on-demand transmit with an on-screen result (console).
- **PING PONG** — bidirectional RTT link test between a pair of units (console).
- **SD card boot-log self-test** — gated by `ENABLE_SD` in
  [`src/console.c`](src/console.c) (off by default). Mounts the FAT card, appends
  a timestamped boot line to `/SD:/BOOT.LOG`, and reads it back to validate the SD
  path end to end.

### Removed

_Retired 2026-09-25 with the streamline to two unit types (see that entry above); kept as history._

- **UART / serial console and all logging on the console build.** The telemetry
  console is now display-only — every `printk` / `LOG_*` call was removed from the
  console sources, and `CONFIG_SERIAL`, `CONFIG_CONSOLE`, `CONFIG_LOG`, and
  `CONFIG_PRINTK` are disabled for this build (in
  `boards/nrf5340dk_nrf5340_cpuapp_display.conf`). All state is shown on the TFT.
  This retired the `PINGPONG,*` CSV trace and the boot/status prints; the
  LoRa-send variant is unchanged and still reports over UART.

### Scripts & SDK preparation

These ready a fresh-from-Nordic NCS v3.2.1 install, which does **not** ship the
native SX126x LoRa driver this project uses. See the README's
[SDK setup](README.md#sdk-setup-fresh-from-nordic) section for the full rationale.

- [`scripts/setup_sdk.sh`](scripts/setup_sdk.sh) — One-shot SDK prep. Swaps the
  vendored driver in [`external/zephyr-lora-driver/`](external/zephyr-lora-driver)
  into `$ZEPHYR_BASE/drivers/lora` (backing up the stock copy to
  `drivers/lora.stock.bak`), then runs `apply.sh`. Idempotent; defaults
  `ZEPHYR_BASE` to `C:/ncs/v3.2.1/zephyr`.
- [`patches/apply.sh`](patches/apply.sh) — Applies the two compat patches with
  `git apply`. Idempotent: it reverse-checks each patch and skips ones already
  applied, and refuses to run if the native driver drop-in is absent.
- [`patches/0001-native-sx126x-spi-dt-spec-arity.patch`](patches/0001-native-sx126x-spi-dt-spec-arity.patch)
  — adds the trailing `delay` argument (`0`) to the native driver's
  `SPI_DT_SPEC_INST_GET` calls (v3.2.1's `spi.h` requires the 3-argument form).
- [`patches/0002-sx126x-base-binding-add-ldo-props.patch`](patches/0002-sx126x-base-binding-add-ldo-props.patch)
  — adds the `regulator-ldo` and `force-ldro` boolean properties to the
  `semtech,sx126x-base` devicetree binding, which the native driver reads.

> [!NOTE]
> The driver swap and patches live in the NCS install, not in this repo, and are
> reverted by any `west update` / SDK reinstall — re-run `scripts/setup_sdk.sh`
> and do a pristine build (`-p always`) afterward.

### Known limitations / next

- **RTT has no host-present signal.** A unit cannot tell whether a capture
  is attached, so `rtt_dropped` (and the LOG page's `r` count) climbs
  harmlessly whenever a soak runs with no host reading. Start
  `rtt_link.py capture` before the soak; the 8 KB up buffer covers only a
  ~2.5 s late start.
- **By default the runner stages right after TxDone (78.7 ms
  stage-to-DIO1).** `lead` fixes that for tests. The guaranteed engine
  deadline is 19.6 ms before the boundary (staging lead sweep, above), so
  live audio should stage by then plus margin until the engine takes the
  payload closer to TX.
- **Latency needs three units.**
  `reconstruct_c2.py --tx` measures the offset between two units' slot
  clocks by common view of a third unit, so a two-unit run reports
  delivery but no latency.

- **Touch calibration is in-RAM only** on the TFT unit. That is deliberate:
  it is a mandatory power-on step lasting the session (see above).
- **TX records carry no frame counter.** `frame_ctr` is a *shared* frame
  number — the master owns it and the secondary adopts it verbatim from each
  beacon, so both units stamp the same value in a given frame, which is what
  makes cross-unit correlation work at all. But the engine does not expose its
  counter to the application, so `soak_log_tx()` writes 0 with `SOAK_F_NO_CTR`
  set. RX-to-RX alignment across the two files works today; pairing a unit's
  own TX against the peer's RX needs the counter added to
  `struct tdma_telemetry`.
- **`frame_ctr` wraps** at 65536 frames — **1.46 h at the current 20 ms
  slot** (it was 3.6 h at the 50 ms bring-up width), which the console's
  2-hour soak preset exceeds. Until this closes, keep runs that need
  cross-file pairing under ~1.4 h; a longer continuous run is still valid but
  only pairwise-alignable up to the first wrap.
  Within-file continuity is already wrap-safe (both firmware and
  decoder use modular deltas and ignore backward jumps as a peer restart);
  absolute cross-file alignment past a wrap would need the decoder to unwrap
  into a monotonic index, which is not built.
- **The 20 ms slot is soaked for up to three units.** The 30-minute
  two-unit acceptance soak passed (2026-08-18, above). Its worst
  |`phase_err`| of 75 µs leaves the 250 µs lock threshold ~3× margin, and
  the unit locked in 1.66 s. A 5-minute three-unit run passed too
  (2026-09-25, above): locks in 1.35 s and 2.79 s, worst 52 µs. With four
  units, a secondary still sees the beacon once per frame but has three
  peers' slots between corrections, and neither the phase envelope nor the
  acquisition transient has been measured there. Card #23 (garbled first
  packet after lock) must land before the four-unit soak: #23 predicts the
  third consecutive RX wraps onto the TX header every frame in a 4-slot
  geometry, which would confound the result. Its first-packet form is now
  confirmed on air (three-unit soak, above). The tone soak (2026-09-25)
  shows its full shape. The first packet's payload is byte-exact the
  previous received packet: its 4-byte header, then its payload bytes 0–35.
  So that RX landed at buffer offset `0x04`, where
  `0x80 + 3 × 44 = 0x104` wraps to. A ramp could not show this, because a
  ramp shifted by 4 bytes is still a ramp. The Codec 2 soak (2026-09-25)
  found a second layout. SEC 1's first packet was the +4 shift. SEC 2's
  was the last 8 bytes of one received packet (SEC 1's chunk 2) followed by
  bytes 4–35 of another (the master's chunk 3). So the wrap lands at more
  than one phase; `reconstruct_c2.py` names both. Because SYNCING → RUNNING is
  one-way, the failure mode to watch for is a unit slow to lock or stuck
  in SYNCING — not one that drops out mid-run.
- **Three soak logs have come back with an 8 KB hole in them.** The master's file from
  the 30 min 50 ms run (commit `337ec96`) contains 128 consecutive records —
  16 sectors, sector-aligned — of garbage plus one sector of stale data, with
  `seq` running 12743 → 12544…12551 → 12872. Decoded naively that reads as
  `PER = 1.597 %` with 145 missed; excluding the hole the same file shows 0
  missed / 0 dup, and the engine counters agree (secondary `tx_done` 8997 vs
  master `rx_done` 8991, i.e. ~6 packets of real loss in 30 minutes). Cause
  not established — the size matches `SOAK_LOG_RING_DEPTH` exactly, but a
  producer-side ring drop leaves a `seq` gap without writing garbage, so this
  is a write-path or card fault, not a drop. It matters because at 20 ms the
  record rate roughly doubles: **check any one-sided loss for a hole before
  reading it as a radio result.**
  It recurred in the master's file from the 20 ms acceptance soak
  (`P0_30M_000.BIN`): again exactly 128
  records, 16 sectors, sector-aligned (file sectors 3668–3683). Five of the
  sectors are zeroed and eleven hold stale data from earlier card contents.
  A naive decode reads PER 26 %, yet the engine counters show no loss:
  master `rx_done` 22,480 = secondary `tx_done` 22,480. Both of those were
  on the master's card. A third came back in SEC 1's log from the
  three-unit soak (2026-09-25): file sectors 271–286, again 128 records
  with 40 zeroed and 88 stale, the same mix as the second. The unit was a
  secondary this time, so the fault follows a card or a unit, not the
  role. Worth checking whether it was the same physical card. Tracked as
  card #25 (master's SD card silently loses writes).
- **Field PER is unmeasured.** The 5-minute bench soak below closed at 0 % loss
  with ~45 dB of margin over SF5/BW500 sensitivity — that validates the stack,
  not the range. Loss behaviour at distance is still unknown.
- **Front end.** The rev 2 shield now carries an OLED + microSD front end for
  the field variant (see "Rev 2 shield / field variant"; bench bring-up
  pending). The TFT breakout used by the two consoles is still jumper-wired
  to the DK headers.
- **Field variant follow-ons:** move the soak runner into a module the TFT
  console and the field unit share; strip UART logging from the field
  variant once it is proven; handle a card swap while mounted, either by a
  remount on a DET change or with `cd-gpios` on the `sdhc` node if this
  tree's `sdhc_spi` supports it (DET is confirmed active-low on P1.12).
