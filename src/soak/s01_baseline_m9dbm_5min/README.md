# s01 — baseline (-9 dBm, 5 min)

First soak: sanity-check the link at the SX1262's **minimum** TX power so later
tests have a worst-case floor to compare against.

| Parameter   | Value                                   |
|-------------|-----------------------------------------|
| TX power    | **-9 dBm** (both units)                 |
| PHY         | SF5 / BW 500 kHz / CR 4-5 / 915 MHz (locked) |
| Duration    | 300 s (5 min)                           |
| CSV cadence | every 5 s                               |
| Roles       | P1.10 jumper: 3V3 = master, open = secondary |

## Build / flash / capture

```sh
west build -b nrf5340dk/nrf5340/cpuapp -p always -d build-soak-s01 -- \
  -DEXTRA_CONF_FILE=src/soak/s01_baseline_m9dbm_5min/s01.conf \
  -DEXTRA_DTC_OVERLAY_FILE=src/soak/role_select.overlay
west flash -d build-soak-s01

python ../tools/ingest_soak.py --port COM7 -o s01_master.csv
```

See [../README.md](../README.md) for the harness, jumper wiring, and CSV schema.

## Reading the result

`per_pct` on the `done` line is the packet error rate against the peer's
one-packet-per-frame expectation. At -9 dBm over a short bench hop expect a low
single-digit PER at most; a high `timeouts` count with `sync=RUNNING` points at
link margin, whereas never reaching `RUNNING` points at the secondary failing to
lock the master's beacon.
