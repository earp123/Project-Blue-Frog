#!/usr/bin/env bash
#
# Apply the vendored NCS SDK patches required to build this project's native
# SX126x LoRa driver against NCS v3.2.1. Safe to re-run (idempotent).
#
# Usage:
#   ZEPHYR_BASE=/path/to/ncs/zephyr ./patches/apply.sh
# or, if ZEPHYR_BASE is unset, it defaults to C:/ncs/v3.2.1/zephyr.
#
# See ../README.md ("SDK setup") for what these do and why.

set -euo pipefail

ZB="${ZEPHYR_BASE:-C:/ncs/v3.2.1/zephyr}"
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -d "$ZB" ]; then
	echo "error: ZEPHYR_BASE not found: $ZB" >&2
	echo "       set ZEPHYR_BASE to your NCS zephyr/ checkout and retry." >&2
	exit 1
fi

# Sanity check: the native driver drop-in must be present. These patches make an
# already-present newer native LoRa driver build; they do not create it.
if [ ! -f "$ZB/drivers/lora/native/sx126x/sx126x.c" ]; then
	echo "error: native SX126x driver not found under $ZB" >&2
	echo "       expected drivers/lora/native/sx126x/sx126x.c (the non-stock" >&2
	echo "       driver drop-in). See README.md." >&2
	exit 1
fi

echo "ZEPHYR_BASE = $ZB"

# --ignore-whitespace: on Windows the repo checkout (and thus the driver tree
# setup_sdk.sh copies into the SDK) carries CRLF line endings while the
# patches are stored with LF; without it git refuses to match the context.
for p in "$DIR"/[0-9][0-9][0-9][0-9]-*.patch; do
	name="$(basename "$p")"
	if git -C "$ZB" apply -p1 --ignore-whitespace --reverse --check "$p" 2>/dev/null; then
		echo "skip  (already applied): $name"
	elif git -C "$ZB" apply -p1 --ignore-whitespace --check "$p" 2>/dev/null; then
		git -C "$ZB" apply -p1 --ignore-whitespace "$p"
		echo "apply (done):           $name"
	else
		echo "ERROR: $name does not apply cleanly to $ZB" >&2
		echo "       the SDK may differ from what this patch expects." >&2
		exit 1
	fi
done

echo "Done. Reconfigure with a pristine build (west build -p always)."
