#!/usr/bin/env python3
"""Staging-lead sweep: find where the engine really stops taking a payload.

The engine takes a staged payload at every RX-slot entry and after every
RxDone, and sends the newest one taken before this unit's TX boundary. So
the last pickup before our slot is either the entry of the slot before ours
(always there, one slot ahead of our boundary) or the RxDone in that slot,
which exists only when a packet from it is heard. A chunk staged after the
last pickup misses its frame: that frame re-sends the previous chunk
(stale_retx), and the missed chunk is picked up just after, then usually
overwritten by the next one before it is sent (lost), or sent a frame late
if the next one is late too.

The runner's `lead <us>` command (rtt_link.py) stages each chunk that long
before the unit's next TX boundary. The data thread polls every 2 ms, so the
lead actually achieved scatters below the setting, and each TX record logs
it. Per chunk this tool works out:

  - on time, late or lost: stage-to-DIO1 minus lead is the air path
    (~8 ms) or a frame more, or the chunk never arrives. The clock offset
    between the two units comes from common view of the third
    (reconstruct_c2.py). Late and lost both count as a missed pickup; on the
    bench, radio loss is negligible next to that;
  - skipping each sender's first chunk (card #23), anything staged before
    the engine ran, and its last END_TRIM chunks (the runs end a frame or
    two apart);
  - whether the sender heard the slot before its own in that frame, i.e.
    whether the RxDone pickup existed. On the bench it always does where
    that slot is occupied; in the field one lost packet removes it, so the
    deadline to design for is the one that holds without it.

Usage:
    lead_sweep.py run --leads 0,30,24,22,21,20.5,20,19.5,16,13,11 \\
        --minutes 2 -o soaks/rtt/lead      # all connected units, clip mode
    lead_sweep.py analyse soaks/rtt/lead/L*

run expects every unit picked, a clip loaded and `mode clip` set.
"""

import argparse
import bisect
import glob
import os
import struct
import subprocess
import sys
from collections import defaultdict

import numpy as np

import reconstruct_c2 as rc
from decode_soak_log import REC_RX, REC_STATS, REC_TX

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT_COUNT = 4
LATE_US = 40000		# stage-to-DIO1 minus lead above this = a frame late
LEAD_CLAMP = 0xFFFF	# the TX record's 16-bit lead saturates here (lead 0)
SYNC_RUNNING = 2
# The three runs end a frame or two apart, so a sender's last chunks go out
# after its preceding unit has stopped, or to receivers no longer listening.
END_TRIM = 8
BIN_US = 500


def run(args):
    import rtt_link
    serials = rtt_link.list_serials()
    if not serials:
        sys.exit("no J-Link emulators connected")
    tool = os.path.join(HERE, "rtt_link.py")
    for lead_ms in args.leads:
        lead = int(round(lead_ms * 1000))
        out = os.path.join(args.out, "L%05d" % lead)
        os.makedirs(out, exist_ok=True)
        for sn in serials:
            r = subprocess.run([sys.executable, tool, "--sn", str(sn), "lead",
                                str(lead)], capture_output=True, text=True)
            if not r.stdout.startswith("ok"):
                sys.exit("%d: lead %d: %s%s" % (sn, lead, r.stdout, r.stderr))
        print("== lead %.1f ms -> %s" % (lead_ms, out), flush=True)
        subprocess.run([sys.executable, tool, "--all", "capture", "-o",
                        out + os.sep, "--soak", str(args.minutes)],
                       check=True)


def stale_retx(recs):
    """stale_retx over the run, from the first and last STATS records."""
    s = [struct.unpack_from("<7I", r.payload)[5] for r in recs
         if r.type == REC_STATS]
    return s[-1] - s[0] if len(s) >= 2 else None


class Unit:
    def __init__(self, path):
        self.path = path
        self.recs, self.meta = rc.load(path)
        self.slot = self.meta["slot_id"]
        self.leads = {}
        _, self.sent, self.rx = rc.tx_sent(path, self.leads)
        self.stage = {(cid, u): t for cid, d in self.sent.items()
                      for u, t in d.items() if t is not None}
        self.stale = stale_retx(self.recs)
        # Chunks whose timing says nothing about staging: any staged before
        # the engine was RUNNING, and the run's first transmitted chunk,
        # which card #23 garbles (CHANGELOG, Known limitations).
        self.skip = set()
        unwrap = {}
        for r in self.recs:
            if r.type != REC_TX:
                continue
            h = rc.header(r.payload)
            if h is None:
                continue
            key = (h[1], unwrap.setdefault(h[1], rc.Unwrap())(h[0]))
            if r.sync != SYNC_RUNNING:
                self.skip.add(key)
        if self.stage:
            order = sorted(self.stage, key=lambda k: k[1])
            self.skip.add(order[0])
            self.skip.update(order[-END_TRIM:])
        # This unit's own RX times of the slot before its own, for the
        # RxDone pickup that may or may not exist in a given frame.
        prev = (self.slot - 1) % SLOT_COUNT
        self.prev_rx = sorted(r.t_us for r in self.recs
                              if r.type == REC_RX and r.slot_id == prev)


def heard_prev(unit, boundary):
    """Did unit hear the slot before its own ahead of this TX boundary?
    Returns (heard, boundary - RxDone us) — unwrap-free: a run is short."""
    t = unit.prev_rx
    i = bisect.bisect_left(t, boundary)
    if i and 0 < rc.s32(boundary - t[i - 1]) < 20000:
        return True, rc.s32(boundary - t[i - 1])
    return False, None


def analyse_step(paths):
    units = [Unit(p) for p in paths]
    out = []
    for s in units:
        others = [u for u in units if u is not s]
        per = {}	# key -> (lead, on_time or None if lost, lat)
        for r in others:
            lat, _, _ = rc.latency_by_chunk(s.slot, r.slot, s.stage, r.rx,
                                            s.rx)
            for k in s.stage:
                if k not in s.leads or k in s.skip:
                    continue
                if k in lat:
                    late = lat[k] - s.leads[k] > LATE_US
                    prev = per.get(k)
                    if prev is None or prev[1] is None:
                        per[k] = (s.leads[k], not late, lat[k])
                elif k not in per:
                    per[k] = (s.leads[k], None, None)
        rows = []
        for k, (lead, on_time, lat) in per.items():
            if lead >= LEAD_CLAMP:
                heard, pickup = None, None	# boundary unknown
            else:
                heard, pickup = heard_prev(s, s.stage[k] + lead)
            rows.append((lead, on_time, lat, heard, pickup))
        out.append((s, rows))
    return out


def ms(x):
    return x / 1000.0


def analyse(args):
    dirs = sorted(d for pat in args.dirs for d in glob.glob(pat)
                  if os.path.isdir(d))
    pooled = defaultdict(list)	# slot -> rows
    print("%-8s %-5s %6s %22s %7s %6s %5s %6s %10s" % (
        "set", "slot", "chunks", "lead achieved ms", "on-time", "late",
        "lost", "stale", "lat ms"))
    for d in dirs:
        paths = sorted(glob.glob(os.path.join(d, "*.bin")))
        if len(paths) < 3:
            print("%s: need three captures, have %d" % (d, len(paths)))
            continue
        setting = os.path.basename(d)
        for s, rows in analyse_step(paths):
            pooled[s.slot] += rows
            leads = np.array([r[0] for r in rows])
            on = [r for r in rows if r[1]]
            late = [r for r in rows if r[1] is False]
            lost = [r for r in rows if r[1] is None]
            lat = np.array([r[2] for r in on]) if on else np.zeros(1)
            def fmt(x):
                return "  >65.5" if x >= LEAD_CLAMP else "%7.2f" % ms(x)
            print("%-8s %-5d %6d %s /%s /%s %7d %6d %5d %6s %10.2f" % (
                setting, s.slot, len(rows), fmt(leads.min()),
                fmt(np.median(leads)), fmt(leads.max()), len(on), len(late),
                len(lost), s.stale, ms(np.median(lat))))

    print()
    print("Per unit, pooled over every set: where chunks start missing their "
          "frame (late or lost), split by whether the unit heard the slot "
          "before its own in that frame.")
    for slot in sorted(pooled):
        rows = [(r[0], r[1] is True) + tuple(r[2:]) for r in pooled[slot]
                if r[0] < LEAD_CLAMP]
        print("slot %d (preceding slot %d):" % (slot, (slot - 1) % SLOT_COUNT))
        for heard in (True, False):
            sub = [r for r in rows if r[3] == heard]
            if not sub:
                print("  heard preceding slot=%-5s no chunks" % heard)
                continue
            late = [r[0] for r in sub if not r[1]]
            ok = [r[0] for r in sub if r[1]]
            line = "  heard preceding slot=%-5s %6d chunks" % (heard, len(sub))
            if late:
                line += (", missed at leads %.2f..%.2f ms" %
                         (ms(min(late)), ms(max(late))))
                above = [x for x in ok if x > max(late)]
                line += (", on time at every lead above %.2f ms (%d chunks)"
                         % (ms(max(late)), len(above)))
                mixed = [x for x in ok if x <= max(late)]
                if mixed:
                    line += (", %d on-time chunks inside the missed range"
                             % len(mixed))
            else:
                line += ", none missed (lowest lead %.2f ms)" % ms(min(ok))
            print(line)
        pick = [r[4] for r in rows if r[4] is not None]
        if pick:
            print("  preceding slot's RxDone: %.2f..%.2f ms before our "
                  "boundary (median %.2f)" % (ms(min(pick)), ms(max(pick)),
                                              ms(float(np.median(pick)))))
        if args.bins:
            bins = defaultdict(lambda: [0, 0])
            for r in rows:
                bins[(r[0] // BIN_US) * BIN_US][0 if r[1] else 1] += 1
            for b in sorted(bins):
                o, l = bins[b]
                if l or (b // BIN_US) % 4 == 0:
                    print("    lead %6.2f..%6.2f ms: on time %5d, missed %5d"
                          % (ms(b), ms(b + BIN_US), o, l))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("--leads", required=True,
                   type=lambda s: [float(x) for x in s.split(",")],
                   help="comma-separated leads in ms (0 = stage at once)")
    p.add_argument("--minutes", type=int, default=2)
    p.add_argument("-o", "--out", required=True)
    p = sub.add_parser("analyse")
    p.add_argument("dirs", nargs="+", help="step directories (globs ok)")
    p.add_argument("--bins", action="store_true",
                   help="print the on-time / late count per 0.5 ms of lead")
    args = ap.parse_args()
    (run if args.cmd == "run" else analyse)(args)


if __name__ == "__main__":
    main()
