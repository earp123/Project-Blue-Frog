# Changelog

All notable changes to this firmware are documented here. The format is loosely
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project is
pre-release, so the current development state lives under **Unreleased** until the
first tagged version.

For hardware wiring, build/flash instructions, and SDK setup, see
[README.md](README.md).

## [Unreleased]

On-device radio-evaluation tooling for the nRF5340 DK + Wio-SX1262 (SX1262).
_Last updated: 2026-06-29._

### Firmware variants

Two variants build from one source tree; exactly one `main()` is linked, chosen
by the Kconfig choice in [`Kconfig`](Kconfig):

- **LoRa send** (`CONFIG_APP_LORA_SEND`, default) — [`src/main.c`](src/main.c).
  Minimal one-shot transmit plus a raw-SPI radio status dump. Radio-only bring-up.
- **Telemetry console** (`CONFIG_APP_CONSOLE`) — [`src/console.c`](src/console.c)
  and its modules. The full touch + DK-button device app described below.

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
  **RESET STATS** clears them. Outstanding pings time out after 1 s. Every event
  is logged to UART as greppable `PINGPONG,*` CSV (see
  [Ping/pong UART schema](#pingpong-uart-schema)).
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
  mutex-guarded stats, the 44-byte packet format, and UART CSV logging.
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

#### Ping/pong UART schema

All lines are prefixed `PINGPONG,` for easy `grep` filtering:

```text
PINGPONG,START
PINGPONG,STOP
PINGPONG,TX_PING,seq=<n>,tx_us=<t>
PINGPONG,RX_PONG,seq=<n>,rx_us=<t>,rtt_us=<r>,rssi=<dbm>,snr=<db>
PINGPONG,RX_PING,seq=<n>,rx_us=<t>,rssi=<dbm>,snr=<db>
PINGPONG,TX_PONG,seq=<n>,tx_us=<t>
PINGPONG,TIMEOUT,seq=<n>
PINGPONG,ABANDONED,seq=<n>        # SEND PING pressed while one was still outstanding
PINGPONG,UNEXPECTED,type=<x>      # malformed packet, or pong with no matching ping
PINGPONG,STATS_RESET
```

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
