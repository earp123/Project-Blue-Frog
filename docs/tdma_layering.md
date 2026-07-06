# TDMA layering findings (M0)

Verification record for the intercom TDMA variant's four-layer architecture:
what the native driver (L0) actually does, how the app-owned command layer
(L1) and TDMA engine (L2) coexist with it, and what was found when auditing
the driver for autonomous runtime behaviour. File/line references are into
the vendored driver (`external/zephyr-lora-driver/drivers/lora/native/sx126x/`),
which is byte-identical to the installed copy under `$ZEPHYR_BASE/drivers/lora`
(modulo line endings) by construction of `scripts/setup_sdk.sh`.

## Where the native driver landed on this machine

- SDK in use: **NCS v3.2.0** at `C:/ncs/v3.2.0/zephyr` (the repo README says
  v3.2.1; this machine has v3.2.0 and v3.3.0 installed, and the nRF Connect
  toolchain bundle `66cdf9b75e` serves both v3.2.0 and v3.2.1). Set
  `ZEPHYR_BASE=C:/ncs/v3.2.0/zephyr` when running `scripts/setup_sdk.sh`.
- The vendored `drivers/lora` tree was already installed there, but the
  compat patches were **not** applied — `git apply` refused them because the
  Windows checkout (`.gitattributes` `text=auto`) copies CRLF files into the
  SDK while the patches are LF. `patches/apply.sh` now passes
  `--ignore-whitespace`, which resolves this; it also globs all numbered
  patches instead of naming 0001/0002.
- Stock NCS v3.2.0's `include/zephyr/drivers/lora.h` predates the vendored
  driver and lacks `SF_5`, the narrow `BW_*` members, `packet_crc_disable`,
  and the `recv_duty_cycle_async`/`airtime` API slots. **No variant of this
  project compiled against it.** New patch
  `patches/0003-lora-h-native-driver-api-compat.patch` brings the header up
  to what the vendored driver (and the existing console/lora-send sources)
  expect, mirroring the newer upstream layout. All three firmware variants
  build after `setup_sdk.sh`.

## L0 driver: DIO1 mechanism and the detach

The driver's interrupt path (the one autonomous runtime SPI path):

- `sx126x_hal_init()` registers a `struct gpio_callback` on the DIO1 pin:
  `gpio_init_callback(&data->dio1_cb, dio1_isr, ...)` + `gpio_add_callback()`
  (`sx126x_hal.c:160-161`). The callback struct lives in driver data:
  `((struct sx126x_data *)dev->data)->hal.dio1_cb` (`sx126x_hal.h:41-45`,
  `sx126x.h:35-36`).
- `dio1_isr` forwards to `data->dio1_callback`, which the driver sets to
  `sx126x_dio1_callback` (`sx126x.c:1479`); that handler does
  `k_work_submit(&data->irq_work)` (`sx126x.c:559-564`) and
  `sx126x_irq_work_handler` performs **SPI transactions on the system
  workqueue** (`sx126x.c:800-834`) — exactly the violation of L2's sole-SPI-
  ownership invariant that had to be neutralized.

**Detach method used:** `gpio_remove_callback()` on the driver's callback
struct, which *is* reachable — `tdma_radio.c` includes the driver-internal
`sx126x.h` (include path added in `CMakeLists.txt` pointing at the installed
copy in `$ZEPHYR_BASE`) and removes `&drv_data->hal.dio1_cb` from the DIO1
port, then registers the TDMA port's own callback and re-enables the pin
interrupt. No `patches/0003`-style detach hook was needed for this. After
removal the driver cannot regain the pin: `gpio_add_callback` for its struct
only happens in `sx126x_hal_init()` (device init), and
`sx126x_hal_set_dio1_callback()` only flips the function pointer and the pin
interrupt config, not the callback registration.

Ordering (in `tdma_radio_init()`): `device_is_ready()` → `lora_config()` →
detach + attach → remaining L1 init → `k_sem_give(&tdma_spi_bus_sem)`. The
radio thread's first act is taking that semaphore, held forever.

## Does `lora_config()` calibrate the image post-TCXO?

**Yes.** `sx126x_chip_init()` (driver device init) configures the TCXO via
DIO3 and runs `Calibrate(ALL)` after it (`sx126x.c:476-490`), and
`sx126x_lora_config()` additionally runs `sx126x_calibrate_image(dev,
config->frequency)` on every call (`sx126x.c:878-882`) with the correct
902-928 MHz band bytes (0xE1/0xE9) for 915 MHz (`sx126x.c:218-220`). L1
therefore does **not** re-issue `CalibrateImage` at init;
`sx126x_cmd_calibrate_image()` exists in L1 for completeness.

## Autonomous-quiescence audit of the driver after init

Checked the whole vendored native driver for anything that could touch the
SPI bus or radio state without an explicit `lora_*` API call:

| Path | Finding |
| --- | --- |
| DIO1 `gpio_callback` → `k_work irq_work` | The one real violation; detached at init (above). `irq_work` is only ever submitted from the DIO1 callback (`sx126x.c:563`), so with the callback removed the work item is dead. |
| `k_timer` / delayed work | None in the driver. |
| PM hooks | `sx126x_pm_action` exists only under `CONFIG_PM_DEVICE` (`sx126x.c:1439-1453`), which is **off** in this build (verified in `.config`); it also only touches GPIOs, not SPI. |
| LoRa shell | `drivers/lora/shell.c` issues runtime `lora_*` calls, but `CONFIG_LORA_SHELL` is **off** (defaults n; the TDMA variant's `select SHELL` does not enable it — verified in `.config`). |
| Sleep-when-idle | `CONFIG_LORA_SX126X_NATIVE_SLEEP` **defaults y** and would have `lora_config()` put the chip to sleep on return (and detach/re-attach DIO1 on its own). The app Kconfig forces it **n** for `APP_TDMA_TEST`, so `lora_config()` returns with the chip in STDBY_RC, configured, and the driver idle. |

Conclusion: after `tdma_init()` completes, the only ways the driver could run
are `lora_*` API calls, which L2 never makes past init. The invariant is
temporal and now holds.

## `sx126x_cmd` (L1) devicetree wiring

`sx126x_cmd.c` resolves its own specs from the same `lora0` node
(`DT_ALIAS(lora0)` → `&lora` on spi2, per the auto-applied
`nrf5340dk_nrf5340_cpuapp.overlay`):

- `SPI_DT_SPEC_GET(RADIO_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0)` —
  same word size/order/CS behaviour as the driver, 500 kHz per the overlay's
  `spi-max-frequency`. The Zephyr SPI subsystem serializes bus users, so
  L0-at-init and L1-at-runtime sharing spi2 is safe.
- `GPIO_DT_SPEC_GET(RADIO_NODE, busy_gpios)` (P1.09) — all BUSY gating lives
  in L1: every transaction is preceded by a bounded BUSY-low spin
  (`sx126x_cmd_wait_busy`, 20 ms budget, timeout is fatal to the engine and
  counted as `busy_timeouts`).
- `GPIO_DT_SPEC_GET(RADIO_NODE, dio1_gpios)` (P1.08) — exported to the port
  layer for the IRQ handover.
- RF_SW (P1.00, `zephyr,user` `rf-sw-gpios`) is configured output-LOW first
  thing in `tdma_radio_init()`, same as the console and lora-send variants.

Init details beyond the plan's M0 list, found necessary while verifying the
driver:

- The DS §15.1 TX-modulation workaround for 500 kHz BW (clear bit 2 of reg
  0x0889) is applied by the driver only in its send path, which is never
  called — L1 applies it once at init; the radio never sleeps so it sticks.
- The driver programs a 200 µs PA ramp in `lora_config()`; L1 re-issues
  `SetPaConfig` + `SetTxParams(+22 dBm, 40 µs)` to match the locked
  invariants.
- Init ends in STDBY_XOSC (TCXO running); `SetRxTxFallbackMode(FS)` keeps the
  PLL locked between slots thereafter.

## Slot timer

TIMER2 on the app core (free: no other user in any variant; the kernel tick
uses RTC1, BLE/802.15.4 live on the net core), enabled in the shared board
overlay with `prescaler = <4>` → 1 MHz, driven via the Zephyr counter API
(`CONFIG_COUNTER_NRF_TIMER`, selected by `APP_TDMA_TEST` via
`CONFIG_COUNTER`). `tdma_port_init()` verifies `counter_get_frequency() ==
1 MHz` at boot. One alarm channel re-arms itself from an accumulating
absolute target; the DIO1 callback timestamps edges with
`counter_get_value()` (software-read jitter: a few µs of GPIOTE ISR latency,
acceptable for the M2 proportional sync and documented in the plan).
