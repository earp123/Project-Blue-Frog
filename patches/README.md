# Vendored NCS SDK patches

These patches make this project's **native SX126x LoRa driver** build against
**NCS v3.2.1**. They modify files **inside the NCS install** (`$ZEPHYR_BASE`),
not this repository — so they are vendored here and applied with a script.

## Apply

> [!TIP]
> **New here?** Run [`scripts/setup_sdk.sh`](../scripts/setup_sdk.sh) instead — it
> installs the vendored driver *and* runs `apply.sh` for you. Use `apply.sh`
> directly only to re-apply the patches when the native driver is already present
> (see [Prerequisite](#prerequisite-the-vendored-lora-driver) below).

```bash
# defaults ZEPHYR_BASE to C:/ncs/v3.2.1/zephyr
ZEPHYR_BASE=/path/to/ncs/zephyr ./patches/apply.sh
```

The script is idempotent (skips already-applied patches) and uses `git apply`
against the NCS `zephyr/` checkout. After applying, do a **pristine** build
(`west build -p always`) — Patch 2 changes a devicetree binding, which is only
picked up on a CMake reconfigure.

## What they fix

`west build` fails without these with:

- `macro "SPI_DT_SPEC_INST_GET" requires 3 arguments, but only 2 given`
- `'..._P_regulator_ldo' undeclared` / `'..._P_force_ldro' undeclared`

| File | Patch | Why |
|------|-------|-----|
| `0001-native-sx126x-spi-dt-spec-arity.patch` | adds the trailing `delay` arg (`0`) to both `SPI_DT_SPEC_INST_GET` calls in `drivers/lora/native/sx126x/sx126x.c` | the native driver uses the newer 2-arg SPI macro; v3.2.1's `spi.h` requires 3 args |
| `0002-sx126x-base-binding-add-ldo-props.patch` | adds the `regulator-ldo` and `force-ldro` boolean properties to `dts/bindings/lora/semtech,sx126x-base.yaml` | the native driver reads these DT props, which don't exist in v3.2.1's binding |

## Prerequisite: the vendored LoRa driver

These patches assume the **newer native LoRa driver is present** in the SDK
(`drivers/lora/native/`). That driver is **not** part of stock NCS v3.2.1, so it
is vendored in `../external/zephyr-lora-driver/` and installed by
`../scripts/setup_sdk.sh`, which copies it into `$ZEPHYR_BASE/drivers/lora` and
*then* runs `apply.sh`.

**For a fresh-from-Nordic SDK, run `scripts/setup_sdk.sh` — not `apply.sh`
directly.** `setup_sdk.sh` does the driver swap first; `apply.sh` only patches
and will refuse to run if the native driver is absent.

## Caveat: these revert on SDK changes

Any `west update`, SDK reinstall, or Toolchain Manager repair reverts these
edits **and the vendored driver swap**. Re-run
[`scripts/setup_sdk.sh`](../scripts/setup_sdk.sh) afterward — running `apply.sh`
alone is not enough, since it refuses once the native driver is gone.
