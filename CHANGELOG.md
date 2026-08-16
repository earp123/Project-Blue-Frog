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
_Last updated: 2026-08-16._

### Firmware variants

Five variants build from one source tree; exactly one `main()` is linked,
chosen by the Kconfig choice in [`Kconfig`](Kconfig):

- **LoRa send** (`CONFIG_APP_LORA_SEND`, default) — [`src/main.c`](src/main.c).
  Minimal one-shot transmit plus a raw-SPI radio status dump. Radio-only bring-up.
- **Telemetry console** (`CONFIG_APP_CONSOLE`) — [`src/console.c`](src/console.c)
  and its modules. The full touch + DK-button device app described below
  (native-driver radio path; kept as-is).
- **Intercom TDMA test** (`CONFIG_APP_TDMA_TEST`) —
  [`src/tdma_app/main.c`](src/tdma_app/main.c) +
  [`src/tdma/`](src/tdma). See "Intercom TDMA radio layer" below.
- **Intercom TDMA field console** (`CONFIG_APP_TDMA_CONSOLE`) —
  [`src/tdma_console/main.c`](src/tdma_console/main.c) + the same
  [`src/tdma/`](src/tdma) engine. See "TDMA field console" below.
- **Soak test** (`CONFIG_APP_SOAK`) — [`src/soak/`](src/soak). Headless,
  UART-CSV bench harness over the same [`src/tdma/`](src/tdma) engine, with a
  nested `SOAK_TEST_*` choice picking the individual test. See "Soak test
  harness" below.

### Intercom TDMA radio layer (M0-M2 implementation)

New product hosted in this tree: the L1/L2 radio layer for the sports-officials
intercom — a fixed 4-slot TDMA broadcast flood over the same SX1262 PHY
(915.0 MHz, SF5 / BW 500 kHz / CR 4-5, 44-byte fixed packets, 12-symbol
preamble, +22 dBm). Slot width 50 ms for bring-up (one constant,
`TDMA_SLOT_DURATION_US`); frame = 4 slots.

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
  RUNNING after 3 beacons under 1 ms error.
- **App** — Kconfig role/slot (`TDMA_ROLE_MASTER`/`TDMA_ROLE_SECONDARY`,
  `TDMA_SLOT_ID`), telemetry print every 5 s over UART, and `tdma` shell
  commands (`tx`, `rx [s]`, `start`, `stop`, `stats`) for M0 manual bring-up.
- Build: `west build -b nrf5340dk/nrf5340/cpuapp -p always -- -DCONFIG_APP_TDMA_TEST=y`
  (+ `-DCONFIG_TDMA_ROLE_SECONDARY=y` for the second unit).

### TDMA field console (2026-07-21)

First "test container": the bench diagnostics sequence above, packaged as an
on-device TFT applet for field soak testing —
[`src/tdma_console/main.c`](src/tdma_console/main.c). Reuses the telemetry
console's display toolkit ([`src/ui_widgets.c`](src/ui_widgets.c)) and touch
calibration ([`src/touch_cal.c`](src/touch_cal.c)); the radio path is
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
  `west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-tdma-console --`
  `-DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_display.overlay`
  `-DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_tdma_console.conf`
  (same display/touch wiring as the telemetry console; the SD stack was
  added to this build on 2026-07-31).

### Soak test harness (2026-07-30)

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
- **Rate budget** — sized for the 20 ms production slot, not today's 50 ms: a
  4-unit 80 ms frame yields 3 RX + 1 TX = 50 records/s = 3.2 KB/s = ~6 sector
  writes/s.
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
rather than an estimate. On-bench confirmation after reflashing both units:
all four soak-screen readings (`dtx`/`drx`, both roles) should collapse to
≈8292 µs.

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
  latency measured" above); `tdma_port_add_phase_adj()` overwrites rather
  than accumulates a pending correction (latent — only one correction is
  issued per beacon today).

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

- **Staged RF config** — [`src/radio_cfg.c`](src/radio_cfg.c) /
  [`.h`](src/radio_cfg.h). Shadow + applied `lora_modem_config`, clamping
  setters/steppers, `radio_cfg_validate()` (datasheet limits), `radio_cfg_apply()`
  (the single commit point), dirty tracking, and value/summary formatters.
- **Payload buffer** — [`src/payload.c`](src/payload.c) /
  [`.h`](src/payload.h). Byte buffer + length, canned presets, hex format/parse.
- **UI toolkit + keypad** — [`src/ui_widgets.c`](src/ui_widgets.c) /
  [`.h`](src/ui_widgets.h). RGB565 blit primitives (CFB glyph data only — see the
  CFB note in the README), hit-testing, button and config value-row widgets, and
  the reusable HEX/DEC keypad modal.
- **Touch calibration** — [`src/touch_cal.c`](src/touch_cal.c) /
  [`.h`](src/touch_cal.h). In-RAM affine transform + a 5-point least-squares
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

- **RX / round-trip** beyond ping/pong, plus **RSSI/SNR** readout on the TX/RX
  screens.
- **ASCII keypad** (the keypad modal is data-driven and architected for a third
  key table).
- **Touch calibration is in-RAM only.** On the field console this is now
  deliberate — a mandatory power-on step lasting the session (see above).
  Elsewhere (and for ping/pong stats) persistence to settings/NVS or SD
  remains unbuilt.
- **TX records carry no frame counter.** `frame_ctr` is a *shared* frame
  number — the master owns it and the secondary adopts it verbatim from each
  beacon, so both units stamp the same value in a given frame, which is what
  makes cross-unit correlation work at all. But the engine does not expose its
  counter to the application, so `soak_log_tx()` writes 0 with `SOAK_F_NO_CTR`
  set. RX-to-RX alignment across the two files works today; pairing a unit's
  own TX against the peer's RX needs the counter added to
  `struct tdma_telemetry`.
- **`frame_ctr` wraps** at 65536 frames — 3.6 h at the current 50 ms slot, but
  **1.46 h at the 20 ms target**, which the console's 2-hour soak preset
  exceeds. Within-file continuity is already wrap-safe (both firmware and
  decoder use modular deltas and ignore backward jumps as a peer restart);
  absolute cross-file alignment past a wrap would need the decoder to unwrap
  into a monotonic index, which is not built.
- **Field PER is unmeasured.** The 5-minute bench soak below closed at 0 % loss
  with ~45 dB of margin over SF5/BW500 sensitivity — that validates the stack,
  not the range. Loss behaviour at distance is still unknown.
- **Transmit is synchronous** on a dedicated thread (no `lora_send_async` /
  LBT / CAD result semantics).
- **Front end** (display/touch/SD) is still jumper-wired to the DK headers,
  pending migration onto the shield.
