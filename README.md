# nRF5340 LoRa Field-Test Device (SX1262)

An open-source field-test device for **long-distance LoRa radio work**, built
around the Nordic **nRF5340** and a Semtech **SX1262** sub-GHz radio. It
currently hosts the radio layer of a sports-officials intercom: a fixed
4-slot TDMA broadcast flood in which every unit transmits once per frame and
hears everyone else.

Each unit pairs the radio with a local display and an SD card, so it is both
a link **endpoint** and an **instrument**: it runs timed soak tests, shows
live link telemetry, and logs every packet to SD for offline decoding. Build
two to four and you have a real multi-unit link to characterise in the field.

<img width="390" height="292" alt="20260629_111703" src="https://github.com/user-attachments/assets/d1b14a37-bc8c-4a43-98fc-f3588b628d0b" /> <img width="422" height="316" alt="20260629_111739" src="https://github.com/user-attachments/assets/71130063-f5c5-46f6-81aa-70893a8e3935" />

For the design history, measured results and known limitations, see
[CHANGELOG.md](CHANGELOG.md).

## Contents

- [Unit types](#unit-types)
- [Building and flashing](#building-and-flashing)
- [Using a unit](#using-a-unit)
- [Radio parameters](#radio-parameters)
- [LoRa radio shield (TFT unit)](#lora-radio-shield-tft-unit)
- [Rev 2 shield (shield unit)](#rev-2-shield-shield-unit)
- [TFT front end (TFT unit)](#tft-front-end-tft-unit)
- [SDK setup (fresh from Nordic)](#sdk-setup-fresh-from-nordic)

## Unit types

The tree builds one firmware per unit type. Both run the same TDMA radio
layer ([`src/tdma/`](src/tdma)) and the same SD soak logger, on an nRF5340
DK.

| Unit | Hardware | Firmware | Overlay + conf |
|---|---|---|---|
| **TFT unit** | Radio shield (rev 1) on Port 1, plus the Hiletgo ILI9341 TFT breakout (touch + microSD) jumper-wired to Port 0 | `CONFIG_APP_TDMA_CONSOLE`, [`src/tdma_console/`](src/tdma_console) | `boards/nrf5340dk_nrf5340_cpuapp_tft.{overlay,conf}` |
| **Shield unit** | Rev 2 shield: SX1262 + SSD1306 128×64 OLED + microSD in one stack | `CONFIG_APP_TDMA_FIELD`, [`src/tdma_field/`](src/tdma_field) | `boards/nrf5340dk_nrf5340_cpuapp_shield.{overlay,conf}` |

- **TFT unit:** touch + DK-button UI on the TFT. Display-only: no UART.
- **Shield unit:** button-only cycle-menu UI on the OLED. UART logging is on
  over the DK's VCOM.

The two types interoperate on air and can be mixed freely in one kit.

## Building and flashing

Each unit type needs **both** of its files, the overlay and the conf,
passed together. A build with only one of them, or neither, stops at CMake
with these two lines. Use `-p always` (pristine) when switching types.

```sh
# TFT unit
west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-tft -- \
  -DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.overlay \
  -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_tft.conf

# Shield unit
west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-shield -- \
  -DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.overlay \
  -DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.conf
```

The board-named radio overlay (`nrf5340dk_nrf5340_cpuapp.overlay`) is applied
automatically in both builds.

For a **tone soak**, which carries a per-slot audio tone instead of the test
ramp, add `-DCONFIG_SOAK_PAYLOAD_TONE=y` after the two files. Build every unit
in the run that way, then turn any unit's log into a WAV with
[`src/tdma_console/tools/reconstruct_tone.py`](src/tdma_console/tools/reconstruct_tone.py)
(see [`docs/tone_payload_test.md`](docs/tone_payload_test.md)).

**On the bench the SD card is optional.** Both builds include the J-Link RTT
bench port (`CONFIG_SOAK_RTT`). It streams every soak record to the host and
takes commands over the DK's own J-Link USB, driven by
[`src/tdma_console/tools/rtt_link.py`](src/tdma_console/tools/rtt_link.py)
(`pip install pylink-square`):

```sh
rtt_link.py --all capture -o soaks/rtt/ --soak 5   # every unit, 5 min, no cards
```

A capture file is a soak log. The same port uploads a Codec 2 clip and
switches the payload (`mode ramp|tone|clip`). The clip tools are
`c2clip.py` and `reconstruct_c2.py`; see
[`docs/rtt_link_c2_transport.md`](docs/rtt_link_c2_transport.md).

Flash with the DK's J-Link serial number when more than one board is on USB
(`nrfutil device list` shows them):

```sh
west flash -d build-shield --dev-id <serial>
```

> [!NOTE]
> A DK that arrives with readback protection (APPROTECT) refuses to attach.
> `west flash --recover` fixes it once, and **erases all flash on both
> cores**. These builds leave debug access open, so later flashes work
> normally. `west flash` defaults to the `nrfutil` runner, which needs its
> `device` command (`nrfutil install device`); `-r jlink` also works.

## Using a unit

Both types behave the same way:

- **Power-on unit pick (mandatory).** Choose **MASTER** (slot 0) or **SEC 1**,
  **SEC 2** or **SEC 3**. This sets the role and the TX slot together. The
  master beacons the frame timing, and each secondary locks to it. Every
  unit in a kit needs its own slot. The choice holds until reboot. The TFT
  unit runs a touch calibration first.
- **SOAK:** pick 5 min, 30 min, 2 h or continuous. Live pages show elapsed
  time, TX/stale, RX with missed/duplicate frames, phase error and ppm,
  RSSI/SNR, the log file, and the staging-latency gauge.
- **TX power:** −9…+22 dBm, applied from the next packet. The TFT unit boots
  at +22 dBm and the shield unit at +0 dBm.
- **SD logging:** every soak writes `<PWR>_<DUR>_NNN.BIN` (e.g.
  `P0_5M_000.BIN`) with one 64-byte record per packet. Decode with
  [`src/tdma_console/tools/decode_soak_log.py`](src/tdma_console/tools/decode_soak_log.py).
  Each file's header records the unit's role and slot.
- **Buttons:** B1 = UP, B2 = DOWN, B3 = OK, B4 = BACK. On the shield unit, B4
  also stops a running soak.

> [!IMPORTANT]
> **Reboot after swapping an SD card.** Card detect is read by the app only,
> so a card changed while mounted is never re-initialised, and every write
> then fails. The shield unit's socket is push-push: it makes no contact
> until it clicks.

## Radio parameters

Fixed in [`src/tdma/tdma.h`](src/tdma/tdma.h); only TX power is adjustable
at runtime.

- Frequency: 915.0 MHz, single channel
- LoRa SF5 / BW 500 kHz / CR 4/5, explicit header, CRC on
- Preamble: 12 symbols (the SX1262 minimum for SF5)
- Packet: 44 bytes (4-byte TDMA header + 40-byte payload), 7,760 µs on air
- Frame: 4 slots × 20 ms = 80 ms. Master in slot 0, secondaries in 1–3.

---

## LoRa radio shield (TFT unit)

The SX1262 (Wio-SX1262) is mounted on a custom Arduino-form-factor shield that
routes its signals to the nRF5340 DK Port 1 header section. The rev 2 shield
keeps exactly the same radio pins.

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
`zephyr,user` node's `rf-sw-gpios` property (P1.00). The native Zephyr driver
only initialises the chip; the TDMA layer owns it after that.

---

## Rev 2 shield (shield unit)

One Arduino-form-factor board carrying the Wio-SX1262 (same pins as above),
a 0.96" SSD1306 OLED and a push-push microSD socket. Fabrication data:
[`Hardware/OLED-SD-SX1262-Shield_2026-08-16.zip`](Hardware/OLED-SD-SX1262-Shield_2026-08-16.zip).
All pins were traced in its netlist and confirmed on the bench.

| Device | Bus | Pins (DK Arduino header) |
|---|---|---|
| microSD | spi4 | SCK **P1.14** (D12), MOSI P1.13 (D11), MISO **P1.15** (D13), CS P1.11 (D9), card-detect P1.12 (D10, active low) |
| SSD1306 128×64 | i2c1 @ 400 kHz | SDA P0.25 (A4), SCL P0.26 (A5), RST P0.07 (A3), address 0x3C |

- **SD SCK/MISO are the reverse of the DK's own `arduino_spi`.** That is how
  the board is routed.
- **The OLED is on A4/A5.** The R3 header's dedicated SDA/SCL pins are not
  connected.
- **No I2C pull-ups on the shield.** The overlay enables the nRF's internal
  ones as a fallback.
- **The I2C driver's concat buffer is raised to 1025 bytes** (a control byte
  plus a full frame). With the 16-byte default, every frame write fails and
  the screen stays blank.

---

## TFT front end (TFT unit)
[https://a.co/d/047jULLv](url)
The TFT unit's front end — ILI9341 TFT, XPT2046 touch, microSD — shares one
SPI bus (**spi4**, the DK's high-speed `arduino_spi` instance on Port 0),
distinct from the radio's spi2 on Port 1. Each device has its own chip
select. **These signals are jumper-wired to the DK Arduino headers.**

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

> [!IMPORTANT]
> **nRF5340 DK board modifications required.** Several of these Port 0 pins are
> wired to on-board peripherals on the DK and must be freed before the front end
> works. The relevant solder bridges are marked on the DK silkscreen (and
> detailed in the nRF5340 DK Hardware User Guide); the exact `SBxx` designators
> vary by board revision.
>
> - **SPI bus — P0.13–P0.18 are shared with the on-board QSPI flash** (MX25R64).
>   SPI SCK/MOSI/MISO and LCD CS / D-C / RESET land on the flash's
>   `QSPI_IO0/IO1/IO2/IO3` + `QSPI_SCK`/`QSPI_CSN`, so the flash must be
>   **disconnected** (cut its solder bridges) — otherwise it loads and contends on
>   the shared bus.
> - **P0.28–P0.31 are the four on-board LEDs** (LED1–LED4). The front end uses
>   P0.28 (touch CS), P0.29 (touch IRQ), and P0.31 (backlight); P0.30 (LED3) is
>   unused. Isolate these LEDs from the GPIOs (per the silkscreen bridges) so the
>   on-board LEDs don't interfere with the touch/backlight lines.
>
> The four DK buttons (P0.23/24/08/09) are **not** modified — they still drive the
> menu UI.

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
- **Drawing stays single-owner.** Input callbacks only set atomics; the main
  loop does every `display_write()`. Touch taps are debounced (one event per
  press, release required) before the loop consumes them.
- **Touch is calibrated by an on-device affine fit**
  (`src/tdma_console/touch_cal.c`). Raw XPT2046 coordinates are offset /
  swapped / inverted relative to the display; a 6-parameter affine corrects
  all of it at once. Calibration is a mandatory power-on step: five crosshair
  taps, a least-squares fit, then a verify step. The fit lives in RAM only.

---

## SDK setup (fresh from Nordic)

This project uses the **native** Zephyr SX126x LoRa driver
(`CONFIG_LORA_MODULE_BACKEND_NATIVE` / `CONFIG_LORA_SX126X_NATIVE`), which is
**not part of stock NCS v3.2.1**. Two repo assets close that gap so the project
builds against a clean SDK:

- `external/zephyr-lora-driver/` — the newer Zephyr `drivers/lora/`
  subsystem (incl. the native SX126x driver), vendored verbatim.
- `patches/` — small compat patches: SPI macro arity (`0001`), the
  `regulator-ldo` / `force-ldro` binding properties (`0002`), and the newer
  `lora.h` API the driver expects (`0003`).

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
   one to `drivers/lora.stock.bak`) and applies the patches.

3. Build one of the two unit types and flash it, as shown in
   [Building and flashing](#building-and-flashing).

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
