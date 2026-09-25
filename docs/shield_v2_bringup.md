# Task: rev 2 shield bring-up — OLED + microSD field variant (`tdma_field`)

**Status:** ready. **Decisions (2026-09-24):** new variant, not a port of the
TFT console; OLED is an SSD1306 0.96" 128×64 on I2C; UI is the essentials
only (role, soak pick, TX power, SD summary); UART logging stays on for
bring-up; soak runner copied from the TFT console now, extracted to a shared
module as a follow-on card; DET is an app-read GPIO, not wired into the SD
driver. Branch `shield_v2`, cut from `TDMA_shim` at `3746f11`.

Read `CHANGELOG.md` (Unreleased) and `docs/tdma_layering.md` first. The
existing `nrf5340dk_nrf5340_cpuapp.overlay` (SX1262 on spi2 / port 1) is
auto-applied and unchanged; everything below is additive.

## Hardware (rev 2 shield on the nRF5340 DK, 3.3 V)

| Device | Bus | Pins |
|---|---|---|
| Wio-SX1262 | spi2 | unchanged (SCK P1.05, MOSI P1.04, MISO P1.01, CS P1.07, RST P1.06, BUSY P1.09, DIO1 P1.08, RF-SW P1.00) |
| microSD | spi4 | SCK **P1.14**, MOSI P1.13, MISO **P1.15**, CS P1.11, DET P1.12 |
| SSD1306 128×64 | i2c1 | SDA P0.25, SCL P0.26, RST P0.07, addr 0x3C |
| DK buttons / LEDs | gpio | board dts (`buttons` node: P0.23/P0.24/P0.08/P0.09; LEDs P0.28–31 now free) |

The TFT/touch bus (P0.13–18, P0.10, P0.28–31) is retired in this variant;
the display overlay/conf and `tdma_console` stay in the tree, untouched and
buildable.

> [!WARNING]
> Two pin facts are unverified against copper and are the only things that
> can stall day one. Each has a 30-second fallback — try it before debugging
> anything else.
> - **SD SCK/MISO.** The DK's default `spi4` (Arduino D13/D12) is SCK P1.15 /
>   MISO P1.14 — the reverse of the assignment above. If the card reports
>   `disk` failure, swap SCK and MISO in the pinctrl group first.
> - **OLED bus.** The R3 header's dedicated SDA/SCL pins are P1.02/P1.03,
>   not A4/A5. If nothing ACKs at 0x3C on P0.25/P0.26, try 0x3D, then move
>   i2c1's pinctrl to P1.02/P1.03.

## The change

### 1. Overlay — `boards/nrf5340dk_nrf5340_cpuapp_shield.overlay`

Passed via `EXTRA_DTC_OVERLAY_FILE` like the display overlay. Contents:

- `chosen { zephyr,display = &ssd1306; }`
- **spi4** re-pinned: new `spi4_sd_default` / `spi4_sd_sleep` pinctrl groups
  (SCK 1.14, MOSI 1.13, MISO 1.15); `cs-gpios = <&gpio1 11 GPIO_ACTIVE_LOW>`;
  the `sdhc0`/`mmc` nodes copied **verbatim** from the display overlay —
  `spi-max-frequency = <4000000>`, `power-delay-ms = <250>`, `disk-name =
  "SD"`. Those two values are the fix for the enumeration failures in the
  2026-07-31 findings; do not "tidy" them back to the binding defaults.
- **i2c1** re-pinned: `i2c1_oled_default` / `_sleep` (`TWIM_SDA` 0.25,
  `TWIM_SCL` 0.26), `clock-frequency = <I2C_BITRATE_FAST>`, and the SSD1306
  node taken from Zephyr's `boards/shields/ssd1306_128x64/` overlay
  (`solomon,ssd1306fb`, `reg = <0x3c>`, 128×64, `multiplex-ratio = <63>`,
  `segment-remap`, `com-invdir`, `prechargep = <0x22>`,
  `reset-gpios = <&gpio0 7 GPIO_ACTIVE_LOW>`).
- `zephyr,user { sd-det-gpios = <&gpio1 12 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>; }`
  — polarity is a guess (push-push socket, 10 k pull-ups on the shield); see
  test step 3. The node merges with the SX1262 overlay's `rf-sw-gpios`.

### 2. Conf — `boards/nrf5340dk_nrf5340_cpuapp_shield.conf`

Start from `nrf5340dk_nrf5340_cpuapp_tdma_console.conf`:

- `CONFIG_APP_TDMA_FIELD=y` (new Kconfig choice, see 3).
- Keep: `DISPLAY`, `CHARACTER_FRAMEBUFFER` (+ default fonts), `INPUT` +
  `INPUT_GPIO_KEYS`, the whole SD/FAT block including `FS_FATFS_EXFAT`,
  `FS_FATFS_LBA64` and **`FS_FATFS_MOUNT_MKFS=n`**, `MAIN_STACK_SIZE=8192`.
- Drop: `MIPI_DBI`. Add: `I2C=y` (SSD1306 selects it, but be explicit).
- **Do not** carry over the `SERIAL/CONSOLE/LOG=n` block. VCOM (P0.20/22) is
  no longer contended, so leave UART logging on for bring-up; stripping it is
  a follow-on once the variant is proven.

### 3. Variant — `CONFIG_APP_TDMA_FIELD`, `src/tdma_field/`

- `Kconfig`: add the choice entry (`select COUNTER`; help text names the
  shield overlay/conf); extend the `LORA_SX126X_NATIVE_SLEEP` default line
  and the CHANGELOG variant list.
- `CMakeLists.txt`: glob `src/tdma_field/*.c`; add
  `src/tdma_console/sd_log.c` and `src/tdma_console/soak_log.c` explicitly
  (both are display-agnostic — no `ui_widgets`/touch references) with
  `src/tdma_console` on the include path. Do **not** glob `src/tdma_console`
  or pull `ui_widgets.c`/`touch_cal.c` in.
- `src/tdma_field/main.c`: copy `soak_start` / `soak_finish` /
  `soak_data_step` / `soak_data_fn` / `soak_data_tid` and `button_cb` from
  `src/tdma_console/main.c` as-is (the data thread at `K_PRIO_COOP(6)` is
  what keeps staging off the UI loop — keep it). `soak_log_start()` is called
  with `dir = NULL` (root only, no drawers). Drawing goes through CFB
  (`cfb_print` / `cfb_framebuffer_finalize`) on the main loop at ≤ 2 Hz
  while a soak runs — a 1 KB frame at 400 kHz is ~25 ms of I2C on the UI
  thread and nothing else; the data thread is unaffected by construction.
  Use the CFB API as it exists in the NCS v3.2.1 tree (check
  `include/zephyr/display/cfb.h` before assuming signatures) and the
  smallest bundled font that gives ≥ 16 columns × ≥ 6 rows on 128×64.

### 4. UI — essentials only

Buttons keep the console's mapping: B1 UP, B2 DOWN, B3 OK, B4 BACK.

- **ROLE** (boot, mandatory): MASTER / SECONDARY, OK to confirm. Locks at the
  first `tdma_init()`, reboot to change — same semantics as the console.
- **HOME** (menu rows, header shows role + card state):
  - `SOAK` → sub-pick 5 min / 30 min / 2 h / continuous, OK starts.
  - `TX PWR` → UP/DOWN steps the value (−9…+22 dBm, clamp), stored for the
    next soak via `tdma_set_tx_power()`; default +0 dBm.
  - `SD` → count of `.BIN` files in the root, the newest file name, and free
    space if `fs_statvfs` is cheap to add as one more `sd_fsop` op on the
    writer thread. Read-only. Listing goes through `sd_fsop_submit(SD_FSOP_LIST,
    "/SD:", …)` + `sd_fsop_poll()` — never a direct FatFs call from the UI.
- **SOAK** (live): elapsed/limit, tx/stale, rx/miss/dup, phase_err/ppm,
  RSSI/SNR, log name + rec/drop (or `LOG FAIL <stage> <errno>`), the
  `lat N/51ms` gauge. B4 = STOP; the limit expiring ends the run exactly as on
  the console (data thread latches `expired`, UI performs the stop).
- Nothing else: no file browser, no drawers, no labels, no keypad, no
  touch-cal step.

## Test (in this order — stop at the first failure)

1. **Build.** All five existing variants plus `tdma_field` build with
   `-p always`. Build line for the new one:
   `west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-tdma-field --`
   `-DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.overlay`
   `-DEXTRA_CONF_FILE=boards/nrf5340dk_nrf5340_cpuapp_shield.conf`
2. **OLED.** Boot prints a splash line and the ROLE screen. Blank → the
   fallbacks in the warning above, in that order. Image offset by ~2 columns
   means the module is actually an SH1106: switch the compatible.
3. **SD.** On entering HOME, mount + `fs_statvfs`/list the root and show the
   result in the header. `disk` failure → swap SCK/MISO. Also log the raw DET
   level with and without a card and fix the polarity flag in the overlay.
4. **Radio.** 5-minute two-unit soak on the shield hardware at +0 dBm,
   decode both cards: `META slot=20000us` on both, lock in single-digit
   seconds, PER 0.000 %, 0 missed / 0 dup, `dtx` ≈ 8292 µs. This is the
   proof that the new bus assignments did not touch the radio path.

## Assess

If 1–4 pass, the shield is the field unit. Record the bring-up in the
CHANGELOG (new "Rev 2 shield / field variant" entry: pins, the two verified
pin facts, whether DET is active-low, soak numbers) and add the variant to
the Firmware variants list. While there: the 2026-08-17 slot-flip entry still
says the acceptance soak is outstanding, but Trello card #6 records it passing
on 2026-08-18 (`docs/P0_30M_000.BIN`, `s_P0_30M_001.BIN`, 22,480 frames each
way, 0 missed / 0 dup) — fold that result in so the CHANGELOG is the truth
again.

## Out of scope / follow-on cards

- Extract the soak runner (`soak_*` + data thread) into a shared module used
  by both consoles.
- Strip UART logging from the field variant once proven.
- `cd-gpios` on the sdhc node if this tree's `sdhc_spi` supports it.
- Card #23 (garbled first packet after lock) must land before the 4-unit
  soak; not part of this task.
