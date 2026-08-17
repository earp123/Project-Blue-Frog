# Soak tests (`CONFIG_APP_SOAK`)

Bench-top radio soak harness. Each test flashes to **both** nRF5340DK + Wio-SX1262
units, they run a fixed-length continuous TDMA exchange, and each streams
machine-ingestible `SOAK,...` CSV lines over the DK's VCOM UART (USB). It reuses
the shared TDMA L1/L2 radio layer in [`src/tdma`](../tdma) unchanged — this
directory only adds the run harness and per-test entry points.

## What's fixed vs. per-test

Locked for every test (single source of truth in [`src/tdma/tdma.h`](../tdma/tdma.h)):
the PHY — **SF5 / BW 500 kHz / CR 4-5 / 915 MHz**, 44-byte frames, 4 slots at
20 ms (80 ms frame). Each test sets its own **TX power**, **duration**, and
**sample cadence** in its `struct soak_params`.

## Role jumper (one image, two units)

Role is read at boot from **P1.10**, not compiled in:

| P1.10            | Role      | TX slot |
|------------------|-----------|---------|
| jumpered to 3V3  | MASTER    | 0 (timing beacon) |
| open (pull-down) | SECONDARY | 1 |

P1.10 is free (the Wio-SX1262 shield uses only P1.00–P1.09) and is broken out on
the DK's port-1 GPIO header, clear of the Arduino shield — 3V3/GND are on the
same block. Wiring lives in [`role_select.overlay`](role_select.overlay); to move
the pin, change that one line. If the GPIO can't be read the firmware fails safe
to SECONDARY (never claims the master timing role by accident).

## Build & flash

Pristine build per unit (same image both times — the jumper decides the role):

```sh
west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-soak-s01 -- \
  -DEXTRA_CONF_FILE=src/soak/s01_baseline_m9dbm_5min/s01.conf \
  -DEXTRA_DTC_OVERLAY_FILE=src/soak/role_select.overlay
west flash -d build-soak-s01
```

`ZEPHYR_BASE` must point at the NCS v3.2.1 tree prepared by
[`scripts/setup_sdk.sh`](../../scripts/setup_sdk.sh) (vendored LoRa driver).

## Capture & ingest

Each unit logs over its own VCOM port. Capture both, then parse:

```sh
# live off a serial port (needs pyserial), stops at the END sentinel
python src/soak/tools/ingest_soak.py --port COM7 -o master_rows.csv

# or parse a saved capture
python src/soak/tools/ingest_soak.py master.log -o master_rows.csv
```

The tool filters the `SOAK,` lines out of any interleaved Zephyr log output,
writes the per-sample rows to CSV, and prints the run metadata + final summary.

### Line format

Every line is prefixed `SOAK,<id>,` so it survives interleaved logging:

- `meta` — role, slot, power, PHY, frame/slot timing, duration, cadence (once)
- `hdr`  — column names for the `row` lines (once)
- `row`  — one per `sample_period_s`: `t_s,role,sync,tx_done,rx_done,crc_err,`
  `bad_hdr,timeouts,stale,busy_to,rx0,rx1,rx2,rx3,rssi,snr,phase_us,ppm`
- `done` — end-of-run summary incl. derived `per_pct` (packet error rate vs. the
  peer's one-packet-per-frame expectation)
- `END`  — capture sentinel; the firmware then idles until reset

## Adding a test

1. `src/soak/sNN_<name>/main.c` — guard the body with a new
   `CONFIG_SOAK_TEST_<NAME>`, fill `struct soak_params`, call `soak_run()`.
2. `src/soak/sNN_<name>/sNN.conf` — `CONFIG_APP_SOAK=y` + your selector.
3. Add the selector to the `choice "Soak test"` block in the root
   [`Kconfig`](../../Kconfig).

CMake already globs `src/soak/**/*.c`, so no build-file edit is needed.
