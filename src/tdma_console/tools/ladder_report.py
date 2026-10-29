#!/usr/bin/env python3
"""Per-rung link report for TX power ladder runs (payload mode LADDER).

Each ladder packet carries the rung and TX power it was sent at
(src/tdma_console/tx_ladder.h), so one unit's log alone gives the link from
its peer: RSSI, SNR and loss per rung, with no clock alignment between logs.
Pass both units' logs of a run for both directions.

    ladder_report.py M_LADR_000.BIN S1_LADR_000.BIN [--distance-ft 50]
                     [--csv rungs.csv]

Per rung and direction:
    rx/exp   packets heard / sent while the receiver was listening (the
             sender's TX index range it heard, so a late start or early stop
             at either end is not counted as loss; partial rungs marked *)
    rssi     median, and min..max, dBm
    step     change in median RSSI from the rung below (4.0 if the PA and
             the receiver are both linear)
    snr      median, dB
    excess   TX dBm - median RSSI - free-space loss at the distance: what
             the two antennas, their mounting, the room and the RF paths
             cost against 0 dBi antennas in free space. Compare it between
             runs; its absolute value carries the RSSI calibration too.
    bad      packets whose check pattern was corrupted although the radio's
             CRC passed
"""

import argparse
import csv
import math
import os
import statistics
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode_soak_log import (PAYLOAD_LADDER, REC_META, REC_NOISE,  # noqa: E402
                             REC_RX, REC_TX, iter_records, noise_desc,
                             parse_meta, parse_noise)

LADDER_MAGIC = 0x4C
FREQ_HZ = 915e6
UNIT = {0: "MASTER", 1: "SEC 1", 2: "SEC 2", 3: "SEC 3"}
TAG = {0: "M", 1: "S1", 2: "S2", 3: "S3"}


def fspl_db(dist_m, freq_hz=FREQ_HZ):
    return 20 * math.log10(dist_m) + 20 * math.log10(freq_hz) - 147.55


def ladder_fields(payload):
    """(rung, dbm, slot, n, bad) for a ladder payload, None for any other."""
    if payload[0] != LADDER_MAGIC:
        return None
    rung, dbm, slot = payload[1], struct.unpack_from("<b", payload, 2)[0], \
        payload[3]
    n = struct.unpack_from("<I", payload, 4)[0]
    bad = any(payload[i] != (n + i) & 0xFF for i in range(8, 40))
    return rung, dbm, slot, n, bad


def load(path):
    """One log: META, noise records, and per sender slot a dict n -> pkt."""
    meta, noise, heard, own_tx = None, [], {}, []
    for rec in iter_records(open(path, "rb").read()):
        if rec.type == REC_META:
            meta = parse_meta(rec.payload)
        elif rec.type == REC_NOISE:
            noise.append(parse_noise(rec.payload))
        elif rec.type in (REC_RX, REC_TX):
            f = ladder_fields(rec.payload)
            if f is None:
                continue
            rung, dbm, slot, n, bad = f
            if rec.type == REC_TX:
                own_tx.append((rung, dbm, n))
                continue
            # A repeat of an n is a stale retransmit: keep the first copy.
            heard.setdefault(rec.slot_id, {}).setdefault(
                n, (rung, dbm, rec.rssi, rec.snr, bad))
    return meta, noise, heard, own_tx


def rung_ranges(rungs, step, tail):
    """(label, lo, hi) TX-index ranges, hi exclusive, the tail last."""
    out = [(str(r + 1), r * step, (r + 1) * step) for r in range(rungs)]
    out.append(("tail", rungs * step, rungs * step + tail))
    return out


def report_direction(pkts, rungs, step, tail, dist_m, rx_tag, tx_tag):
    """Print one sender -> receiver table; returns its rows."""
    lo_n, hi_n = min(pkts), max(pkts)
    loss_free = fspl_db(dist_m)
    rows, prev_med = [], None
    print("  %s -> %s: heard TX %d..%d of the sender's run" % (
        tx_tag, rx_tag, lo_n, hi_n))
    print("  rung  txdBm    rx/exp    loss   rssi  [min..max]   step"
          "   snr  excess  bad")
    for label, lo, hi in rung_ranges(rungs, step, tail):
        a, b = max(lo, lo_n), min(hi, hi_n + 1)
        exp = max(0, b - a)
        got = [pkts[n] for n in range(lo, hi) if n in pkts]
        if not got and not exp:
            continue
        dbm = got[0][1] if got else None
        rssi = [p[2] for p in got]
        snr = [p[3] for p in got]
        bad = sum(p[4] for p in got)
        loss = 100.0 * (exp - len(got)) / exp if exp else 0.0
        med = statistics.median(rssi) if rssi else None
        snr_med = statistics.median(snr) if snr else None
        step_db = (med - prev_med) if (med is not None and prev_med is not None
                                       and label != "tail") else None
        excess = (dbm - med - loss_free) if med is not None else None
        part = "*" if exp < hi - lo else " "
        print("  %4s  %+5s  %4d/%-4d%s %5.1f%%  %5s  [%s]  %5s  %4s  %6s  %3d"
              % (label, dbm if dbm is not None else "?", len(got), exp, part,
                 loss, "%.1f" % med if med is not None else "-",
                 "%d..%d" % (min(rssi), max(rssi)) if rssi else "-",
                 "%+.1f" % step_db if step_db is not None else "--",
                 "%.1f" % snr_med if snr_med is not None else "-",
                 "%.1f" % excess if excess is not None else "-", bad))
        rows.append(dict(tx_unit=tx_tag, rx_unit=rx_tag, rung=label,
                         tx_dbm=dbm, rx=len(got), exp=exp,
                         loss_pct=round(loss, 2),
                         rssi_med=med, rssi_min=min(rssi) if rssi else None,
                         rssi_max=max(rssi) if rssi else None,
                         snr_med=snr_med,
                         excess_db=round(excess, 1) if excess is not None
                         else None, bad=bad))
        if label != "tail" and med is not None:
            prev_med = med

    # Linearity across the full rungs: slope of median RSSI on TX dBm.
    pts = [(r["tx_dbm"], r["rssi_med"]) for r in rows
           if r["rung"] != "tail" and r["rssi_med"] is not None]
    if len(pts) >= 2:
        mx = sum(p[0] for p in pts) / len(pts)
        my = sum(p[1] for p in pts) / len(pts)
        sxx = sum((p[0] - mx) ** 2 for p in pts)
        slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx
        print("  slope %.2f dB/dB over %d rungs (1.00 = linear);"
              " free-space loss at %.1f m: %.1f dB" % (slope, len(pts),
                                                       dist_m, loss_free))
    return rows


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="LADR_NNN.BIN from one or both"
                    " units of a run")
    ap.add_argument("--distance-ft", type=float, default=50.0,
                    help="antenna separation (default 50 ft)")
    ap.add_argument("--csv", help="write the per-rung rows here")
    args = ap.parse_args()
    dist_m = args.distance_ft * 0.3048

    all_rows = []
    for path in args.logs:
        meta, noise, heard, own_tx = load(path)
        if meta is None:
            print("%s: no META record, skipped" % path)
            continue
        if meta["payload_mode"] != PAYLOAD_LADDER:
            print("%s: not a ladder run (payload_mode %d), skipped" % (
                path, meta["payload_mode"]))
            continue
        rungs = meta["ladder_rungs"]
        step = meta["ladder_step_frames"]
        tail = max([n for _, _, n in own_tx] + [0]) + 1 - rungs * step
        tail = max(tail, 0)
        me = meta["slot_id"]
        print("%s: %s (slot %d)" % (os.path.basename(path), UNIT.get(me, me),
                                    me))
        for nf in noise:
            print("  noise   " + noise_desc(nf))
        if not noise:
            print("  noise   (no NOISE record)")
        print("  own TX  %d ladder packets, %d rungs x %d + tail %d" % (
            len(own_tx), rungs, step, tail))
        for slot in sorted(heard):
            # The sender's tail length is not in this log; assume ours.
            all_rows += report_direction(heard[slot], rungs, step,
                                         max(tail, 1), dist_m,
                                         TAG.get(me, str(me)),
                                         TAG.get(slot, str(slot)))
        if not heard:
            print("  heard no ladder packets")
        print()

    if args.csv and all_rows:
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(all_rows[0]))
            w.writeheader()
            w.writerows(all_rows)
        print("wrote %d rows -> %s" % (len(all_rows), args.csv))


if __name__ == "__main__":
    main()
