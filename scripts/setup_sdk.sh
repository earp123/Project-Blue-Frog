#!/usr/bin/env bash
#
# Prepare a fresh-from-Nordic NCS v3.2.1 SDK to build this project.
#
# Stock NCS v3.2.1 does NOT ship the native Zephyr SX126x LoRa driver this
# project uses. This script makes a clean SDK buildable in two steps:
#
#   1. Swap in the vendored newer LoRa driver subsystem
#      (external/zephyr-lora-driver/drivers/lora -> $ZEPHYR_BASE/drivers/lora),
#      backing up the stock one to drivers/lora.stock.bak.
#   2. Apply the compat patches in patches/ (SPI macro arity + binding props).
#
# Idempotent and safe to re-run (e.g. after a `west update` or SDK reinstall
# reverts the SDK tree).
#
# Usage:
#   ZEPHYR_BASE=/path/to/ncs/zephyr ./scripts/setup_sdk.sh
# Defaults ZEPHYR_BASE to C:/ncs/v3.2.1/zephyr if unset.

set -euo pipefail

ZB="${ZEPHYR_BASE:-C:/ncs/v3.2.1/zephyr}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$REPO/external/zephyr-lora-driver/drivers/lora"

if [ ! -d "$ZB" ]; then
	echo "error: ZEPHYR_BASE not found: $ZB" >&2
	echo "       set ZEPHYR_BASE to your NCS zephyr/ checkout and retry." >&2
	exit 1
fi
if [ ! -d "$VENDOR" ]; then
	echo "error: vendored driver missing: $VENDOR" >&2
	exit 1
fi

echo "ZEPHYR_BASE = $ZB"

# 1. Install the vendored LoRa driver subsystem (back up the stock one once).
BK="$ZB/drivers/lora.stock.bak"
if [ ! -e "$BK" ] && [ -d "$ZB/drivers/lora" ]; then
	cp -r "$ZB/drivers/lora" "$BK"
	echo "backed up stock drivers/lora -> drivers/lora.stock.bak"
fi
rm -rf "$ZB/drivers/lora"
cp -r "$VENDOR" "$ZB/drivers/lora"
echo "installed vendored drivers/lora ($(find "$VENDOR" -type f | wc -l) files)"

# 2. Apply the compat patches (SPI arity + binding props).
export ZEPHYR_BASE="$ZB"
"$REPO/patches/apply.sh"

echo
echo "SDK ready. Build with a pristine configure, e.g.:"
echo "  west build -b nrf5340dk/nrf5340/cpuapp -p always"
