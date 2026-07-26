#!/usr/bin/env python3
"""Ingest a captured soak-test UART log into a tidy per-sample CSV + summary.

The soak firmware (src/soak) streams lines prefixed "SOAK,<id>,..." over UART.
This parser pulls just those lines out of an otherwise noisy capture (Zephyr
LOG output, boot banner, etc.), writes the per-sample rows to a CSV, and echoes
the run metadata and final summary.

Usage:
    # from a saved capture
    python ingest_soak.py capture.log [-o rows.csv]

    # straight off the serial port (needs pyserial: pip install pyserial)
    python ingest_soak.py --port COM7 --baud 115200 -o rows.csv

Only the standard library is required for file parsing; --port additionally
needs pyserial. Reading stops at the "SOAK,<id>,END" sentinel.
"""

import argparse
import csv
import sys

# Column order emitted by soak_run.c's "hdr"/"row" lines (after t_s..).
ROW_FIELDS = [
    "id", "t_s", "role", "sync", "tx_done", "rx_done", "crc_err", "bad_hdr",
    "timeouts", "stale", "busy_to", "rx0", "rx1", "rx2", "rx3", "rssi", "snr",
    "phase_us", "ppm",
]


def parse_lines(lines):
    """Yield ('meta'|'row'|'done'|'error', payload) for each SOAK line."""
    for raw in lines:
        line = raw.strip()
        if not line.startswith("SOAK,"):
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        test_id, kind = parts[1], parts[2]
        yield kind, test_id, parts[3:]
        if kind == "END":
            break


def rows_from(records):
    meta = {}
    rows = []
    summary = {}
    for kind, test_id, fields in records:
        if kind == "meta":
            meta = {"id": test_id}
            meta.update(_kv(fields))
        elif kind == "row":
            # fields are the values after "row"; prepend id for a full record.
            values = [test_id] + fields
            rows.append(dict(zip(ROW_FIELDS, values)))
        elif kind == "done":
            summary = {"id": test_id}
            summary.update(_kv(fields))
        elif kind == "error":
            summary = {"id": test_id, "error": ",".join(fields)}
    return meta, rows, summary


def _kv(fields):
    out = {}
    for f in fields:
        if "=" in f:
            k, v = f.split("=", 1)
            out[k] = v
    return out


def read_serial(port, baud):
    try:
        import serial  # type: ignore
    except ImportError:
        sys.exit("--port needs pyserial: pip install pyserial")
    with serial.Serial(port, baud, timeout=1) as ser:
        buf = ""
        while True:
            chunk = ser.read(256).decode("utf-8", "replace")
            if chunk:
                buf += chunk
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    yield line
                    if line.strip().endswith(",END"):
                        return


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", nargs="?", help="captured UART log file")
    ap.add_argument("--port", help="serial port to read live (e.g. COM7)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("-o", "--out", help="write per-sample rows to this CSV")
    args = ap.parse_args()

    if args.port:
        source = read_serial(args.port, args.baud)
    elif args.logfile:
        source = open(args.logfile, "r", encoding="utf-8", errors="replace")
    else:
        source = sys.stdin

    meta, rows, summary = rows_from(parse_lines(source))

    if meta:
        print("meta:   ", ", ".join(f"{k}={v}" for k, v in meta.items()))
    print(f"samples: {len(rows)}")
    if summary:
        print("summary:", ", ".join(f"{k}={v}" for k, v in summary.items()))

    if args.out and rows:
        with open(args.out, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=ROW_FIELDS)
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {len(rows)} rows -> {args.out}")


if __name__ == "__main__":
    main()
