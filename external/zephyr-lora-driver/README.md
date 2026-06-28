# Vendored Zephyr LoRa driver subsystem

This is the `drivers/lora/` subsystem from a **newer upstream Zephyr** than the
one in NCS v3.2.1. It is vendored here because stock NCS v3.2.1 does **not**
ship the native SX126x LoRa driver (`drivers/lora/native/`) this project builds
against.

`scripts/setup_sdk.sh` installs this tree into a fresh SDK
(`$ZEPHYR_BASE/drivers/lora`, replacing the stock subsystem) and then applies
the compat patches in `../../patches/`.

## Contents

The full `drivers/lora/` tree (32 files): the native SX126x driver
(`native/sx126x/`), plus the newer loramac-node and lora-basics-modem backends
and the top-level `CMakeLists.txt` / `Kconfig*` that wire them up. The whole
subsystem is replaced wholesale to avoid mismatches between the newer build
glue and leftover stock files.

This project selects the **native** backend
(`CONFIG_LORA_MODULE_BACKEND_NATIVE`); the other backends are included only so
the subsystem's CMake/Kconfig is self-consistent.

## Relationship to `patches/`

The vendored source here is kept **pristine** (as upstream). The two changes
needed to build it against NCS v3.2.1 live in `../../patches/` and are applied
by `setup_sdk.sh` after copying:

- `0001-native-sx126x-spi-dt-spec-arity.patch` — SPI macro arity.
- `0002-sx126x-base-binding-add-ldo-props.patch` — binding props (this one
  targets `dts/bindings/`, outside this tree).

## License & provenance

Apache-2.0 (Zephyr Project). All original copyright/license headers are retained
in each file; the native driver carries `Copyright (c) 2026 Carlo Caione`.

Provenance: copied from the newer Zephyr `drivers/lora/` present in this
project's working NCS install; it postdates the `ncs-v3.2.1` tag. The exact
upstream commit is not pinned here — the files are vendored verbatim so the
build is deterministic without a network fetch. If you later want to track a
specific upstream revision, record its SHA in this file.
