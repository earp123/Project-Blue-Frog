# nRF5340 LoRa Field-Test Device (SX1262)

An open-source, dual-telemetry device for **long-distance LoRa radio testing**,
built around the Nordic **nRF5340** and a Semtech **SX1262** sub-GHz radio.

Each unit pairs the SX1262 radio link with a local touchscreen + SD-card
console, so it acts as both a telemetry **endpoint** (transmit/receive frames
over the air) and a local telemetry **instrument** (drive tests, observe radio
state, and log results on-device). Build a pair and you have both ends of a real
long-range link to characterise in the field.

<img width="390" height="292" alt="20260629_111703" src="https://github.com/user-attachments/assets/d1b14a37-bc8c-4a43-98fc-f3588b628d0b" /> <img width="422" height="316" alt="20260629_111739" src="https://github.com/user-attachments/assets/71130063-f5c5-46f6-81aa-70893a8e3935" />



For a feature-by-feature breakdown of the current firmware — every HOME-menu
item, the helper scripts, and the test modes — see
[CHANGELOG.md](CHANGELOG.md).

## Contents

- [Project goal](#project-goal)
- [Hardware so far](#hardware-so-far)
- [Status](#status)
- [Firmware variants](#firmware-variants)
- [Building and flashing](#building-and-flashing)
- [Radio parameters](#radio-parameters)
- [LoRa radio shield](#lora-radio-shield)
- [Display / touch / SD front end](#display--touch--sd-front-end)
- [SDK setup (fresh from Nordic)](#sdk-setup-fresh-from-nordic)

## Project goal

A dynamic, field-usable **toolbox for exercising the SX1262**: configurable RF
parameters, on-device control via buttons/touch, live status on the TFT, and
logging to SD — all without a laptop tethered in the field.

The hardware and firmware are being built out **in parallel**. The SX1262
already lives on a custom Arduino-compatible shield; the display/touch/SD front
end is currently jumper-wired to the DK headers and will be migrated onto the
shield as the design matures. The end state is a single stacked shield on the
nRF5340 DK that turns it into a self-contained LoRa field-test handset.

## Hardware so far

- **Host:** Nordic nRF5340 DK.
- **Radio:** Semtech SX1262 (Seeed **Wio-SX1262** module) — **embedded on a
  custom Arduino-compatible shield** that routes the radio's SPI + control
  signals to the DK's Port 1 header section. A TXS0108E level shifter and a
  33 µF decoupling cap on the radio supply are part of the shield (see
  [LoRa radio shield](#lora-radio-shield)).
- **Front end (display/touch/SD):** ILI9341 320×240 TFT, XPT2046 resistive
  touch, and a microSD slot on a shared SPI bus. **Currently jumper-wired** to
  the DK's Port 0 Arduino headers — not yet on a shield (see
  [Display / touch / SD front end](#display--touch--sd-front-end)).
- **Controls:** the DK's four on-board buttons drive the menu UI.

## Status

- **LoRa TX confirmed working** on the nRF5340 DK + Wio-SX1262.
- **Display, touch, and buttons working**; SD path implemented (validated with a
  card present).
- **On-device radio evaluation tooling** (console variant): configure the RF
  parameters (freq / SF / BW / CR / power / preamble / CRC / IQ / network), build
  the TX payload via an on-screen hex keypad or canned presets, and fire SX1262
  transmits — all from a touch + DK-button screen UI, with transmits on a
  background thread so the UI stays live.
- Next: RX / round-trip testing, RSSI/SNR readout, an ASCII keypad, and migrating
  the front end onto the shield.

## Firmware variants

The project builds two firmware variants from one tree. Exactly one `main()`
is linked, chosen by the Kconfig choice in `Kconfig`
(`CONFIG_APP_LORA_SEND` / `CONFIG_APP_CONSOLE`):

- **LoRa send** (default) — `src/main.c`. Minimal one-shot transmit plus a
  radio IRQ / device-error dump over SPI. Useful for radio-only bring-up.
- **Telemetry console** — `src/console.c` (screen router / main loop) with
  `src/radio_cfg.c` (staged modem config), `src/payload.c` (TX payload +
  presets), and `src/ui_widgets.c` (display primitives + reusable keypad
  modal). The full device app: a touch + DK-button screen UI to configure the
  radio, build payloads, and run SX1262 transmits.

`prj.conf` holds the common + LoRa configuration. The front-end (display,
touch, SD, fonts) Kconfig lives in the companion file
`boards/nrf5340dk_nrf5340_cpuapp_display.conf` so the LoRa-only variant does
not pull in the display stack / FATFS.

## Building and flashing

**LoRa send** variant (board-named overlay is auto-applied):

```sh
west build -b nrf5340dk/nrf5340/cpuapp
west flash
```

**Telemetry console** variant — add the front-end overlay *and* its companion
config (both, together). `-p always` (pristine) is required when switching
variants because the Kconfig choice and devicetree change:

```sh
west build -b nrf5340dk/nrf5340/cpuapp -p always -- \
  -DEXTRA_DTC_OVERLAY_FILE="boards/nrf5340dk_nrf5340_cpuapp_display.overlay" \
  -DEXTRA_CONF_FILE="boards/nrf5340dk_nrf5340_cpuapp_display.conf"
west flash
```

Both overlays end up applied for the console build: the board-named LoRa overlay
is auto-detected and the front-end overlay is added via
`EXTRA_DTC_OVERLAY_FILE`. The radio (spi2/Port 1) and front end (spi4/Port 0)
never overlap.

## Radio parameters

The LoRa-send variant hard-codes these in `src/main.c`. The console variant
uses them as the **power-on defaults** of its staged config (`src/radio_cfg.c`)
and lets you change them on-device, committing via **APPLY** (see
[Telemetry console behaviour](#telemetry-console-behaviour)). Console-editable
ranges in parentheses:

- Frequency: 915 MHz (902–928 MHz)
- Bandwidth: 500 kHz (125 / 250 / 500 kHz)
- Spreading factor: SF5 (SF5–SF12; LoRa-send selects via `LORA_SPREADING_FACTOR`)
- Coding rate: 4/5 (4/5–4/8)
- Preamble: 16 symbols (6–65535; the SX1262 minimum for SF5 and SF6 is 12)
- TX power: +4 dBm (−9…+22 dBm)
- Payload: 16 bytes (editable, up to 255)

---

## LoRa radio shield

The SX1262 (Wio-SX1262) is mounted on a custom Arduino-form-factor shield that
routes its signals to the nRF5340 DK Port 1 header section.

### Wiring (nRF5340 DK → Wio-SX1262)

| Signal   | nRF5340 pin | Wio-SX1262 pin   |
| -------- | ----------- | ---------------- |
| SPI SCK  | P1.05       | SCK              |
| SPI MOSI | P1.04       | MOSI             |
| SPI MISO | P1.01       | MISO             |
| SPI CS   | P1.07       | NSS              |
| RESET    | P1.06       | RESET            |
| BUSY     | P1.09       | BUSY             |
| DIO1     | P1.08       | DIO1             |
| RF_SW    | P1.00       | RF_SW            |
| VCC      | 3V3         | VCC (+ 33µF cap) |
| GND      | GND         | GND              |

Notes:

- **RF_SW** (P1.00) is driven by the firmware as a GPIO output and held LOW
  (at GND) for both TX and RX on this module. This is the correct level for the
  Wio-SX1262 antenna switch — it does not need to be toggled.
- **DIO2** is internally bridged to TXEN on the Wio-SX1262 — no external wiring.
  `dio2-tx-enable` in the overlay lets the chip drive TXEN itself.
- **DIO3** drives the onboard TCXO — no external wiring; configured via
  `dio3-tcxo-voltage`.
- **uart1** is disabled in the overlay so that P1.01 is free for MISO.
- **33µF decoupling capacitor** on the Wio-SX1262 VCC pin is required. The TX
  current transient on the SX1262 power amplifier causes a supply droop
  sufficient to cause intermittent TX failures without it.

Devicetree (`nrf5340dk_nrf5340_cpuapp.overlay`, SX1262 node):

```dts
lora: sx1262@0 {
    compatible = "semtech,sx1262";
    reg = <0>;
    reset-gpios    = <&gpio1 6 GPIO_ACTIVE_LOW>;
    busy-gpios     = <&gpio1 9 GPIO_ACTIVE_HIGH>;
    dio1-gpios     = <&gpio1 8 GPIO_ACTIVE_HIGH>;
    dio2-tx-enable;
    dio3-tcxo-voltage = <SX126X_DIO3_TCXO_1V8>;
    tcxo-power-startup-delay-ms = <10>;
    spi-max-frequency = <500000>;
};
```

The radio is selected through the `lora0` alias; `RF_SW` is exposed via the
`zephyr,user` node's `rf-sw-gpios` property (P1.00).

---

## Display / touch / SD front end
[https://a.co/d/047jULLv](url)
The console front end — ILI9341 TFT, XPT2046 touch, microSD — shares one SPI
bus (**spi4**, the DK's high-speed `arduino_spi` instance on Port 0), distinct
from the radio's spi2 on Port 1. Each device has its own chip select. **These
signals are currently jumper-wired to the DK Arduino headers** and are slated to
move onto the shield.

### Pin assignments

| Signal        | nRF5340 pin | Device pin                       |
| ------------- | ----------- | -------------------------------- |
| SPI SCK       | P0.13       | SCK (all)                        |
| SPI MOSI      | P0.14       | MOSI/SDI (all)                   |
| SPI MISO      | P0.15       | MISO/SDO (all)                   |
| LCD CS        | P0.16       | ILI9341 CS                       |
| LCD D/C       | P0.17       | ILI9341 D/C                      |
| LCD RESET     | P0.18       | ILI9341 RESET                    |
| LCD backlight | P0.31       | Backlight (active high)          |
| TOUCH CS      | P0.28       | XPT2046 CS                       |
| TOUCH IRQ     | P0.29       | XPT2046 IRQ (active low, pull-up)|
| SD CS         | P0.10       | microSD CS                       |

CS lines are indexed by each device's `reg` on the spi4 node: reg 0 = display
(P0.16), reg 1 = touch (P0.28), reg 2 = SD (P0.10).

Controls: the DK's four buttons (gpio-keys, P0.23/24/08/09 → `INPUT_KEY_0..3`)
map to **B1=UP, B2=DOWN, B3=OK, B4=BACK**.

### Implementation notes / deviations

- **Touch binding is** `xptek,xpt2046` (not `ti,tsc2046`) in this Zephyr. It
  reports via the input subsystem (`INPUT_ABS_X/Y`, `INPUT_BTN_TOUCH`); the
  IRQ is edge-to-active (falling, `GPIO_ACTIVE_LOW | GPIO_PULL_UP`).
- **ILI9341 RESET is active-low.** The MIPI-DBI driver leaves reset de-asserted
  after its pulse, so `reset-gpios` must be `GPIO_ACTIVE_LOW` — otherwise the
  panel is held in reset and stays blank even though SPI writes "succeed".
- **CFB cannot drive the ILI9341.** Zephyr's character framebuffer is
  monochrome-only (1 bpp); the ILI9341 is RGB565/RGB888. The app enables CFB only
  to link its (correct) bundled font glyph data, then blits that into RGB565
  itself with `display_write()` (white/black, byte-order agnostic).
- The display uses the **MIPI DBI** subsystem: a `zephyr,mipi-dbi-spi`
  controller wraps spi4 and owns the D/C and RESET sidebands; the
  `ilitek,ili9341` node sits under it.
- **Config is stage-then-apply.** The menu edits an in-RAM `lora_modem_config`
  shadow; `radio_cfg_apply()` is the single commit point (one `lora_config()`
  call). `radio_cfg_validate()` holds the documented limits (902–928 MHz,
  −9…+22 dBm, preamble ≥ 6, and ≥ 12 for SF5/SF6) and is where any further
  SX126x SF/BW pairing constraints belong.
- **Transmit is synchronous on a dedicated thread**, not `lora_send_async`.
  The thread already keeps the UI non-blocking, so this avoids `CONFIG_POLL` /
  `k_poll_signal` for no behavioural gain; switch to the async API if LBT/CAD
  result semantics are wanted later.
- **Drawing stays single-owner.** Input callbacks (touch / buttons) and the TX
  thread only set atomics or signal a semaphore; the main loop does every
  `display_write()`. Touch taps are debounced (one event per press, release
  required) before the loop consumes them.
- **Touch is calibrated by an on-device affine fit** (`src/touch_cal.c`). The
  panel is rotated 90°, so raw XPT2046 coordinates are offset / swapped / inverted
  relative to the display; a 6-parameter affine
  (`screen = A·raw + B·raw + C` per axis) corrects all of it at once. The
  **TOUCH CAL** screen collects five crosshair taps, least-squares-fits the
  transform, and offers a verify step. The ISR latches the *raw* sample;
  `touch_cal_apply()` runs in the main loop, so calibration capture sees raw
  coordinates. The fit is in-RAM (re-run after each reflash; persistence to
  settings/NVS is a follow-up). The DK buttons drive the entire UI — including
  reaching TOUCH CAL — so uncalibrated touch is never a lock-out.

---

## SDK setup (fresh from Nordic)

This project uses the **native** Zephyr SX126x LoRa driver
(`CONFIG_LORA_MODULE_BACKEND_NATIVE` / `CONFIG_LORA_SX126X_NATIVE`), which is
**not part of stock NCS v3.2.1**. Two repo assets close that gap so the project
builds against a clean SDK:

- `external/zephyr-lora-driver/` — the newer Zephyr `drivers/lora/`
  subsystem (incl. the native SX126x driver), vendored verbatim.
- `patches/` — two small compat patches: SPI macro arity
  (`0001`) and the `regulator-ldo` / `force-ldro` binding properties
  (`0002`).

### Onboarding (what to do with a fresh Nordic SDK)

1. Install **NCS v3.2.1 + toolchain** via nRF Connect for Desktop → *Toolchain
   Manager* (provides `west`, CMake, Ninja, the Zephyr SDK/GCC). Install
   **SEGGER J-Link** too (the DK's on-board debugger).

2. Prepare the SDK from this repo (idempotent; defaults `ZEPHYR_BASE` to
   `C:/ncs/v3.2.1/zephyr`):

   ```sh
   ZEPHYR_BASE=/path/to/ncs/zephyr ./scripts/setup_sdk.sh
   ```

   This swaps the vendored `drivers/lora/` into the SDK (backing up the stock
   one to `drivers/lora.stock.bak`) and applies the two patches.

3. Build (pristine) and flash:

   ```sh
   west build -b nrf5340dk/nrf5340/cpuapp -p always       # LoRa-send variant
   west flash -r jlink
   ```

   For the telemetry console, add the front-end overlay + conf as shown in
   [Building and flashing](#building-and-flashing).

> [!NOTE]
> `west flash` defaults to the `nrfutil` runner; if its `device` plugin
> is not installed, flashing fails. Use `west flash -r jlink` (or run
> `nrfutil install device` once).

**Symptoms if the SDK was not prepared:**

- `macro "SPI_DT_SPEC_INST_GET" requires 3 arguments, but only 2 given`
- `'..._P_regulator_ldo' undeclared` / `'..._P_force_ldro' undeclared`
- silently building the wrong LoRa backend (the native `CONFIG_*` symbols
  don't exist on a clean SDK and are dropped with a warning)

> [!WARNING]
> These changes live in the NCS install, not in this repo. Any `west update`,
> SDK reinstall, or Toolchain Manager repair reverts them — just re-run
> `scripts/setup_sdk.sh` and do a pristine build. Patch 2 changes a
> devicetree binding, which is only picked up on a CMake reconfigure
> (`-p always`).
