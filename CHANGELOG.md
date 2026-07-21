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
_Last updated: 2026-07-21._

### Firmware variants

Four variants build from one source tree; exactly one `main()` is linked,
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

- **One image for both units**: role (MASTER/SECONDARY) is picked on the HOME
  screen and locks at the first soak start (`tdma_init()` is once-only);
  reboot to change. Defaults to SECONDARY — two unconfigured units just
  listen instead of both beaconing into slot 0.
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
- **TOUCH CAL** — the console's 5-point calibrate/verify flow, unchanged.
- The soak screen is laid out for a future slot-width selector (the engine
  still pins `TDMA_SLOT_DURATION_US` at compile time; making it runtime is
  the planned next engine change for width evaluation).
- Build:
  `west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-tdma-console --`
  `-DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_display.overlay`
  `-DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_tdma_console.conf`
  (same display/touch wiring as the telemetry console; the SD stack is left
  out of this build).

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
- **Open items before tightening to 20 ms slots**: TX-start latency differs
  between master and secondary by ~1.1 ms and `TDMA_TX_START_LATENCY_US` only
  reflects the master's value (feeds the sync math, currently absorbed by the
  50 ms guard band); `tdma_port_add_phase_adj()` overwrites rather than
  accumulates a pending correction (latent — only one correction is issued
  per beacon today).

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
- **Touch calibration and ping/pong stats are in-RAM only** — no persistence
  across reflash/reboot yet (settings/NVS or SD is the planned store).
- **Transmit is synchronous** on a dedicated thread (no `lora_send_async` /
  LBT / CAD result semantics).
- **Front end** (display/touch/SD) is still jumper-wired to the DK headers,
  pending migration onto the shield.
