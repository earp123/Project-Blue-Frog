nRF5340 LoRa Field-Test Device (SX1262)
=======================================

An open-source, dual-telemetry device for **long-distance LoRa radio testing**,
built around the Nordic **nRF5340** and a Semtech **SX1262** sub-GHz radio.

Each unit pairs the SX1262 radio link with a local touchscreen + SD-card
console, so it acts as both a telemetry **endpoint** (transmit/receive frames
over the air) and a local telemetry **instrument** (drive tests, observe radio
state, and log results on-device). Build a pair and you have both ends of a real
long-range link to characterise in the field.

Project goal
------------

A dynamic, field-usable **toolbox for exercising the SX1262**: configurable RF
parameters, on-device control via buttons/touch, live status on the TFT, and
logging to SD — all without a laptop tethered in the field.

The hardware and firmware are being built out **in parallel**. The SX1262
already lives on a custom Arduino-compatible shield; the display/touch/SD front
end is currently jumper-wired to the DK headers and will be migrated onto the
shield as the design matures. The end state is a single stacked shield on the
nRF5340 DK that turns it into a self-contained LoRa field-test handset.

Hardware so far
---------------

- **Host:** Nordic nRF5340 DK.
- **Radio:** Semtech SX1262 (Seeed **Wio-SX1262** module) — **embedded on a
  custom Arduino-compatible shield** that routes the radio's SPI + control
  signals to the DK's Port 1 header section. A TXS0108E level shifter and a
  33 µF decoupling cap on the radio supply are part of the shield (see
  `LoRa radio shield`_).
- **Front end (display/touch/SD):** ILI9341 320×240 TFT, XPT2046 resistive
  touch, and a microSD slot on a shared SPI bus. **Currently jumper-wired** to
  the DK's Port 0 Arduino headers — not yet on a shield (see
  `Display / touch / SD front end`_).
- **Controls:** the DK's four on-board buttons drive the menu UI.

Status
------

- **LoRa TX confirmed working** on the nRF5340 DK + Wio-SX1262.
- **Display, touch, and buttons working**; SD path implemented (validated with a
  card present).
- **Async telemetry transmit** is wired into the on-device menu: selecting
  *Send Telemetry* fires an SX1262 transmit on a background thread while the UI
  stays live and shows radio state.
- Next: richer payloads, RX/round-trip testing, and migrating the front end onto
  the shield.

Firmware variants
-----------------

The project builds two firmware variants from one tree. Exactly one ``main()``
is linked, chosen by the Kconfig choice in ``Kconfig``
(``CONFIG_APP_LORA_SEND`` / ``CONFIG_APP_DISPLAY_SHIELD_TEST``):

- **LoRa send** (default) — ``src/main.c``. Minimal one-shot transmit plus a
  radio IRQ / device-error dump over SPI. Useful for radio-only bring-up.
- **Telemetry console** — ``src/shield_test.c``. The full device app: TFT menu,
  touch, DK-button navigation, and async SX1262 transmit driven from the menu.

``prj.conf`` holds the common + LoRa configuration. The front-end (display,
touch, SD, fonts) Kconfig lives in the companion file
``boards/nrf5340dk_nrf5340_cpuapp_display.conf`` so the LoRa-only variant does
not pull in the display stack / FATFS.

Building and flashing
---------------------

**LoRa send** variant (board-named overlay is auto-applied):

.. code-block:: console

   west build -b nrf5340dk/nrf5340/cpuapp
   west flash

**Telemetry console** variant — add the front-end overlay *and* its companion
config (both, together). ``-p always`` (pristine) is required when switching
variants because the Kconfig choice and devicetree change:

.. code-block:: console

   west build -b nrf5340dk/nrf5340/cpuapp -p always -- \
     -DEXTRA_DTC_OVERLAY_FILE="boards/nrf5340dk_nrf5340_cpuapp_display.overlay" \
     -DEXTRA_CONF_FILE="boards/nrf5340dk_nrf5340_cpuapp_display.conf"
   west flash

Both overlays end up applied for the console build: the board-named LoRa overlay
is auto-detected and the front-end overlay is added via
``EXTRA_DTC_OVERLAY_FILE``. The radio (spi2/Port 1) and front end (spi4/Port 0)
never overlap.

Radio parameters
----------------

Shared by both variants (LoRa-send sets them in ``src/main.c``; the console sets
them in ``src/shield_test.c``):

- Frequency: 915 MHz
- Bandwidth: 500 kHz
- Spreading factor: SF5 (SF6 also used for testing — ``LORA_SPREADING_FACTOR``)
- Coding rate: 4/5
- Preamble: 12 symbols (the SX1262 minimum for SF5 and SF6)
- TX power: +4 dBm
- Payload: 16 bytes

================================================================================

LoRa radio shield
=================

The SX1262 (Wio-SX1262) is mounted on a custom Arduino-form-factor shield that
routes its signals to the nRF5340 DK Port 1 header section.

Wiring (nRF5340 DK → Wio-SX1262)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

==========  ============  ===================
Signal      nRF5340 pin   Wio-SX1262 pin
==========  ============  ===================
SPI SCK     P1.05         SCK
SPI MOSI    P1.04         MOSI
SPI MISO    P1.01         MISO
SPI CS      P1.07         NSS
RESET       P1.06         RESET
BUSY        P1.09         BUSY
DIO1        P1.08         DIO1
RF_SW       P1.00         RF_SW
VCC         3V3           VCC (+ 33µF cap)
GND         GND           GND
==========  ============  ===================

Notes:

- **RF_SW** (P1.00) is driven by the firmware as a GPIO output and held LOW
  (at GND) for both TX and RX on this module. This is the correct level for the
  Wio-SX1262 antenna switch — it does not need to be toggled.
- **DIO2** is internally bridged to TXEN on the Wio-SX1262 — no external wiring.
  ``dio2-tx-enable`` in the overlay lets the chip drive TXEN itself.
- **DIO3** drives the onboard TCXO — no external wiring; configured via
  ``dio3-tcxo-voltage``.
- **uart1** is disabled in the overlay so that P1.01 is free for MISO.
- **33µF decoupling capacitor** on the Wio-SX1262 VCC pin is required. The TX
  current transient on the SX1262 power amplifier causes a supply droop
  sufficient to cause intermittent TX failures without it.

Devicetree (``nrf5340dk_nrf5340_cpuapp.overlay``, SX1262 node):

.. code-block:: dts

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

The radio is selected through the ``lora0`` alias; ``RF_SW`` is exposed via the
``zephyr,user`` node's ``rf-sw-gpios`` property (P1.00).


Display / touch / SD front end
==============================

The console front end — ILI9341 TFT, XPT2046 touch, microSD — shares one SPI
bus (**spi4**, the DK's high-speed ``arduino_spi`` instance on Port 0), distinct
from the radio's spi2 on Port 1. Each device has its own chip select. **These
signals are currently jumper-wired to the DK Arduino headers** and are slated to
move onto the shield.

Pin assignments
~~~~~~~~~~~~~~~~

=============  ============  =================
Signal         nRF5340 pin   Device pin
=============  ============  =================
SPI SCK        P0.13         SCK (all)
SPI MOSI       P0.14         MOSI/SDI (all)
SPI MISO       P0.15         MISO/SDO (all)
LCD CS         P0.16         ILI9341 CS
LCD D/C        P0.17         ILI9341 D/C
LCD RESET      P0.18         ILI9341 RESET
LCD backlight  P0.31         Backlight (active high)
TOUCH CS       P0.28         XPT2046 CS
TOUCH IRQ      P0.29         XPT2046 IRQ (active low, pull-up)
SD CS          P0.10         microSD CS
=============  ============  =================

CS lines are indexed by each device's ``reg`` on the spi4 node: reg 0 = display
(P0.16), reg 1 = touch (P0.28), reg 2 = SD (P0.10).

Controls: the DK's four buttons (gpio-keys, P0.23/24/08/09 → ``INPUT_KEY_0..3``)
map to **B1=UP, B2=DOWN, B3=OK, B4=BACK**.

Telemetry console behaviour
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

On boot it brings up the display, touch, buttons and radio, then shows a menu
with a live telemetry block. Selecting **Send Telemetry** + **OK** fires an
SX1262 transmit on a background thread; the screen shows ``STATE`` cycling
IDLE → SENDING → SENT/FAILED, a success counter, and the last return code, while
the UI stays responsive. Everything is also logged to UART.

On the TFT::

   TELEMETRY
   [Send Telemetry]
    Settings
    About

   LORA: READY
   STATE: SENT
   TX#:3  RC:0
   Sending...

   B1:UP B2:DN B3:OK B4:BK

Implementation notes / deviations
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Touch binding is** ``xptek,xpt2046`` (not ``ti,tsc2046``) in this Zephyr. It
  reports via the input subsystem (``INPUT_ABS_X/Y``, ``INPUT_BTN_TOUCH``); the
  IRQ is edge-to-active (falling, ``GPIO_ACTIVE_LOW | GPIO_PULL_UP``).
- **ILI9341 RESET is active-low.** The MIPI-DBI driver leaves reset de-asserted
  after its pulse, so ``reset-gpios`` must be ``GPIO_ACTIVE_LOW`` — otherwise the
  panel is held in reset and stays blank even though SPI writes "succeed".
- **CFB cannot drive the ILI9341.** Zephyr's character framebuffer is
  monochrome-only (1 bpp); the ILI9341 is RGB565/RGB888. The app enables CFB only
  to link its (correct) bundled font glyph data, then blits that into RGB565
  itself with ``display_write()`` (white/black, byte-order agnostic).
- The display uses the **MIPI DBI** subsystem: a ``zephyr,mipi-dbi-spi``
  controller wraps spi4 and owns the D/C and RESET sidebands; the
  ``ilitek,ili9341`` node sits under it.

================================================================================

SDK setup (fresh from Nordic)
=============================

This project uses the **native** Zephyr SX126x LoRa driver
(``CONFIG_LORA_MODULE_BACKEND_NATIVE`` / ``CONFIG_LORA_SX126X_NATIVE``), which is
**not part of stock NCS v3.2.1**. Two repo assets close that gap so the project
builds against a clean SDK:

- ``external/zephyr-lora-driver/`` — the newer Zephyr ``drivers/lora/``
  subsystem (incl. the native SX126x driver), vendored verbatim.
- ``patches/`` — two small compat patches: SPI macro arity
  (``0001``) and the ``regulator-ldo`` / ``force-ldro`` binding properties
  (``0002``).

Onboarding (what to do with a fresh Nordic SDK)
-----------------------------------------------

1. Install **NCS v3.2.1 + toolchain** via nRF Connect for Desktop → *Toolchain
   Manager* (provides ``west``, CMake, Ninja, the Zephyr SDK/GCC). Install
   **SEGGER J-Link** too (the DK's on-board debugger).
2. Prepare the SDK from this repo (idempotent; defaults ``ZEPHYR_BASE`` to
   ``C:/ncs/v3.2.1/zephyr``):

   .. code-block:: console

      ZEPHYR_BASE=/path/to/ncs/zephyr ./scripts/setup_sdk.sh

   This swaps the vendored ``drivers/lora/`` into the SDK (backing up the stock
   one to ``drivers/lora.stock.bak``) and applies the two patches.
3. Build (pristine) and flash:

   .. code-block:: console

      west build -b nrf5340dk/nrf5340/cpuapp -p always       # LoRa-send variant
      west flash -r jlink

   For the telemetry console, add the front-end overlay + conf as shown in
   `Building and flashing`_.

.. note::

   ``west flash`` defaults to the ``nrfutil`` runner; if its ``device`` plugin
   is not installed, flashing fails. Use ``west flash -r jlink`` (or run
   ``nrfutil install device`` once).

**Symptoms if the SDK was not prepared:**

- ``macro "SPI_DT_SPEC_INST_GET" requires 3 arguments, but only 2 given``
- ``'..._P_regulator_ldo' undeclared`` / ``'..._P_force_ldro' undeclared``
- silently building the wrong LoRa backend (the native ``CONFIG_*`` symbols
  don't exist on a clean SDK and are dropped with a warning)

.. warning::

   These changes live in the NCS install, not in this repo. Any ``west update``,
   SDK reinstall, or Toolchain Manager repair reverts them — just re-run
   ``scripts/setup_sdk.sh`` and do a pristine build. Patch 2 changes a
   devicetree binding, which is only picked up on a CMake reconfigure
   (``-p always``).
