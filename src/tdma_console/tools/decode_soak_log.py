#!/usr/bin/env python3
"""Decode a binary soak log (SOAKnnn.BIN) written by the TDMA field console.

The firmware writes fixed 64-byte records so the on-device cost is a memcpy and
each FAT sector holds exactly 8 records. All the text formatting happens here,
where it is free.

Usage:
    python decode_soak_log.py SOAK000.BIN                 # summary only
    python decode_soak_log.py SOAK000.BIN -o rows.csv     # + per-record CSV
    python decode_soak_log.py SOAK000.BIN --payload       # include payload hex

Pair two units' logs by matching RX payload content against the peer's TX
payloads; frame_ctr gives continuity within one unit's own view.
"""

import argparse
import csv
import struct
import sys
from collections import Counter

REC_SIZE = 64
REC_FMT = "<BBBBIHhb3sII40s"
META_FMT = "<IHBBIIIIBBHbBBBI"
MAGIC = 0x4B414F53  # "SOAK"

REC_META, REC_RX, REC_TX, REC_STATS = 1, 2, 3, 4
TYPE_NAME = {REC_META: "meta", REC_RX: "rx", REC_TX: "tx", REC_STATS: "stats"}
SYNC_NAME = {0: "STOPPED", 1: "SYNCING", 2: "RUNNING"}
F_NO_CTR = 1 << 0

STATS_FIELDS = ["tx_done", "rx_done", "rx_crc_err", "rx_bad_header",
                "slot_timeouts", "stale_retx", "busy_timeouts",
                "rx_ok", "missed", "dup"]

CSV_FIELDS = ["seq", "type", "uptime_ms", "t_us", "slot_id", "sync",
              "frame_ctr", "rssi", "snr"] + STATS_FIELDS + ["payload"]


def parse_meta(payload):
    (magic, version, role, slot_id, freq_hz, slot_us, frame_us, toa_us,
     sf, cr, bw_khz, tx_power, slot_count, payload_len, preamble,
     uptime_ms) = struct.unpack(META_FMT, payload[:struct.calcsize(META_FMT)])
    if magic != MAGIC:
        return None
    return {
        "version": version,
        "role": "master" if role == 0 else "secondary",
        "slot_id": slot_id,
        "freq_hz": freq_hz,
        "slot_us": slot_us,
        "frame_us": frame_us,
        "toa_us": toa_us,
        "phy": "SF%d/BW%d/CR4-%d" % (sf, bw_khz, cr + 4),
        "tx_power_dbm": tx_power,
        "slot_count": slot_count,
        "payload_len": payload_len,
        "preamble_syms": preamble,
        "uptime_ms": uptime_ms,
    }


def payload_errors(payload, n):
    """The firmware's test pattern is payload[i] = base + i (mod 256).

    The base is recovered by majority vote over every byte rather than read
    from payload[0]: each position votes (payload[i] - i) & 0xFF, and the
    winner is the base. Trusting payload[0] alone would mean a single
    corrupted first byte made all 40 bytes look wrong.

    Returns (bad_bytes, bad_bits). Note this only catches corruption that
    survived the radio's CRC -- with packet CRC on, damaged frames are dropped
    by the modem and never reach the log, so a clean result here is expected
    rather than informative. To measure true BER, disable the packet CRC so
    corrupted frames still surface.
    """
    votes = Counter((payload[i] - i) & 0xFF for i in range(n))
    base = votes.most_common(1)[0][0]
    bad_bytes = bad_bits = 0
    for i in range(n):
        exp = (base + i) & 0xFF
        if payload[i] != exp:
            bad_bytes += 1
            bad_bits += bin(payload[i] ^ exp).count("1")
    return bad_bytes, bad_bits


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile")
    ap.add_argument("-o", "--out", help="write per-record CSV here")
    ap.add_argument("--payload", action="store_true",
                    help="include payload hex in the CSV")
    args = ap.parse_args()

    blob = open(args.logfile, "rb").read()
    total = len(blob) // REC_SIZE
    if len(blob) % REC_SIZE:
        print("note: %d trailing bytes ignored (partial record)"
              % (len(blob) % REC_SIZE), file=sys.stderr)

    meta = None
    rows = []
    counts = Counter()
    rssi_vals, snr_vals = [], []
    last_stats = None
    seq_prev = None
    seq_gaps = 0
    ctr_by_slot = {}
    missed = dup = 0
    bad_bytes_tot = bad_bits_tot = 0

    for i in range(total):
        rec = blob[i * REC_SIZE:(i + 1) * REC_SIZE]
        (rtype, slot_id, sync, flags, t_us, frame_ctr, rssi, snr, _rsvd,
         seq, uptime_ms, payload) = struct.unpack(REC_FMT, rec)

        counts[rtype] += 1

        # A gap in seq means the on-device ring dropped records.
        if seq_prev is not None and seq != seq_prev + 1:
            seq_gaps += seq - seq_prev - 1
        seq_prev = seq

        row = {f: "" for f in CSV_FIELDS}
        row.update(seq=seq, type=TYPE_NAME.get(rtype, rtype),
                   uptime_ms=uptime_ms, t_us=t_us, slot_id=slot_id,
                   sync=SYNC_NAME.get(sync, sync))

        if rtype == REC_META:
            meta = parse_meta(payload)
            rows.append(row)
            continue

        if rtype == REC_STATS:
            vals = struct.unpack("<10I", payload[:40])
            last_stats = dict(zip(STATS_FIELDS, vals))
            row.update(last_stats)
            # STATS reuses rssi/snr for phase error and ppm.
            row.update(rssi=rssi, snr=snr)
            rows.append(row)
            continue

        # RX / TX
        n = meta["payload_len"] if meta else 40
        if rtype == REC_RX:
            row.update(frame_ctr=frame_ctr, rssi=rssi, snr=snr)
            rssi_vals.append(rssi)
            snr_vals.append(snr)

            prev = ctr_by_slot.get(slot_id)
            if prev is not None:
                delta = (frame_ctr - prev) & 0xFFFF
                if delta == 0:
                    dup += 1
                elif 1 < delta < 0x8000:
                    missed += delta - 1
            ctr_by_slot[slot_id] = frame_ctr

            bb, bt = payload_errors(payload, n)
            bad_bytes_tot += bb
            bad_bits_tot += bt
        elif not (flags & F_NO_CTR):
            row.update(frame_ctr=frame_ctr)

        if args.payload:
            row["payload"] = payload[:n].hex()
        rows.append(row)

    # ---- report ----
    print("file:     %s (%d records)" % (args.logfile, total))
    if meta:
        print("meta:     role=%s slot=%d %s %+ddBm freq=%d" % (
            meta["role"], meta["slot_id"], meta["phy"],
            meta["tx_power_dbm"], meta["freq_hz"]))
        print("timing:   slot=%dus frame=%dus toa=%dus slots=%d" % (
            meta["slot_us"], meta["frame_us"], meta["toa_us"],
            meta["slot_count"]))
    else:
        print("meta:     MISSING (no valid META record)")

    print("records:  rx=%d tx=%d stats=%d meta=%d" % (
        counts[REC_RX], counts[REC_TX], counts[REC_STATS], counts[REC_META]))
    if seq_gaps:
        print("WARNING:  %d records dropped on-device (seq gaps) --"
              " card could not keep up" % seq_gaps)

    if counts[REC_RX]:
        print("link:     rssi min/mean/max %d/%.1f/%d dBm   snr %d/%.1f/%d dB" % (
            min(rssi_vals), sum(rssi_vals) / len(rssi_vals), max(rssi_vals),
            min(snr_vals), sum(snr_vals) / len(snr_vals), max(snr_vals)))
        recvd = counts[REC_RX]
        expected = recvd + missed
        per = (100.0 * missed / expected) if expected else 0.0
        print("continuity: received=%d missed=%d dup=%d  PER=%.3f%%" % (
            recvd, missed, dup, per))
        print("payload:  %d bad bytes / %d bad bits across %d packets" % (
            bad_bytes_tot, bad_bits_tot, recvd))

    if last_stats:
        print("engine:   " + " ".join("%s=%d" % (k, v)
                                      for k, v in last_stats.items()))

    if args.out:
        with open(args.out, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
            w.writeheader()
            w.writerows(rows)
        print("wrote %d rows -> %s" % (len(rows), args.out))


if __name__ == "__main__":
    main()
