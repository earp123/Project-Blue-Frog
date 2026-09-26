#!/usr/bin/env python3
"""Synthetic-capture checks for reconstruct_c2.py.

Builds soak logs record by record, exactly as the firmware writes them (META
first, then RX/TX/STATS, seq monotonic), for a secondary in slot 1 hearing
the master (slot 0) and SEC 2 (slot 2). Each case injects one fault and
checks it lands in the right bucket:

    clean, loss (single + burst), dup (stale re-transmit), stale (same index,
    new bytes), out-of-order, card #23's shifted first packet, an RTT/ring seq
    gap, corrupted bytes, chunk-index wrap, TX pairing, and latency through a
    common-view clock offset with crystal drift.

Run from anywhere: python test_reconstruct_c2.py
"""

import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from decode_soak_log import MAGIC, META_FMT, REC_FMT	# noqa: E402

SLOT_US, FRAME_US = 20000, 80000
REC_META, REC_RX, REC_TX = 1, 2, 3
F_NO_CTR, F_TX_T = 1, 2
AIR_US = 7900		# boundary to DIO1 in the synthetic RX times


def make_clip(seed, chunks=50):
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(chunks * 32))


def payload(clip, n):
    cid = zlib.crc32(clip) & 0xFFFF
    off = (n % (len(clip) // 32)) * 32
    return bytes([0xC2, 0]) + struct.pack("<HH", n & 0xFFFF, cid) + \
        b"\0\0" + clip[off:off + 32]


class Log:
    """A soak log for one unit, built in time order.

    The unit's slot clock reads clock_off + true time * (1 + ppm / 1e6):
    every unit's counter is its own.
    """

    def __init__(self, slot, clip_chunks, clock_off=0, ppm=0.0):
        self.recs = []
        self.seq = 0
        self.slot = slot
        self.clock_off = clock_off
        self.ppm = ppm
        meta = struct.pack(META_FMT, MAGIC, 2, 0 if slot == 0 else 1, slot,
                           915000000, SLOT_US, FRAME_US, 7760, 5, 1, 500, 0,
                           4, 40, 8, 0, 2, 0, clip_chunks)
        self.add(REC_META, slot, 0, 0, meta.ljust(40, b"\0"))

    def clock(self, t_true):
        return int(round(self.clock_off + t_true * (1 + self.ppm / 1e6)))

    def add(self, rtype, slot, t_us, ctr, pl, skip_seq=0, flags=0):
        self.seq += skip_seq
        self.recs.append(struct.pack(REC_FMT, rtype, slot, 2, flags,
                                     t_us & 0xFFFFFFFF, ctr & 0xFFFF, -80, 8,
                                     b"\0\0\0", self.seq, 0, pl))
        self.seq += 1

    def rx(self, frame, slot, pl, skip_seq=0):
        t = 1_000_000 + frame * FRAME_US + slot * SLOT_US + AIR_US
        self.add(REC_RX, slot, self.clock(t), frame, pl, skip_seq)

    def tx(self, pl, stage_true=None):
        """A TX record; with stage_true, a stage time (SOAK_F_TX_T)."""
        if stage_true is None:
            self.add(REC_TX, self.slot, 0, 0, pl, flags=F_NO_CTR)
        else:
            self.add(REC_TX, self.slot, self.clock(stage_true), 0, pl,
                     flags=F_NO_CTR | F_TX_T)

    def write(self, path):
        with open(path, "wb") as f:
            f.write(b"".join(self.recs))


def run(log, clips, tmp, extra=()):
    path = os.path.join(tmp, "cap.bin")
    log.write(path)
    args = [sys.executable, os.path.join(HERE, "reconstruct_c2.py"), path,
            "-o", tmp]
    for i, c in enumerate(clips):
        cp = os.path.join(tmp, "clip%d.c2" % i)
        with open(cp, "wb") as f:
            f.write(c)
        args += ["--clip", cp]
    out = subprocess.run(args + list(extra), capture_output=True, text=True)
    if out.returncode:
        raise AssertionError(out.stderr)
    return out.stdout


def slot_line(out, slot):
    """The stats line of one peer's block."""
    lines = out.splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith("slot %d " % slot))
    block = []
    for l in lines[i + 1:]:
        if l.startswith("slot ") or not l.startswith(" "):
            break
        block.append(l.strip())
    return "\n".join(block)


def expect(block, *needles):
    for n in needles:
        if n not in block:
            raise AssertionError("expected %r in:\n%s" % (n, block))


A, B = make_clip(1), make_clip(2)	# slot 0's and slot 2's clips
N = 120					# frames per synthetic run


def base(**kw):
    """A clean run with hooks: kw[name](frame, log) may replace a frame."""
    log = Log(1, 50)
    for f in range(N):
        if "slot0" in kw and kw["slot0"](f, log):
            pass
        else:
            log.rx(f, 0, payload(A, f))
        log.tx(payload(make_clip(3), f))
        if "slot2" in kw and kw["slot2"](f, log):
            pass
        else:
            log.rx(f, 2, payload(B, f))
    return log


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


@case
def clean(tmp):
    out = run(base(), [A, B], tmp)
    for s in (0, 2):
        b = slot_line(out, s)
        expect(b, "placed %d  missing 0 (log 0)  dup 0  stale 0  "
                  "out-of-order 0  foreign 0" % N,
               "0 mismatched chunks, 0 byte errors",
               "disagrees with chunk index at 0 boundaries")


@case
def loss(tmp):
    drop = {10, 40, 41, 42}
    out = run(base(slot0=lambda f, log: f in drop), [A, B], tmp)
    b = slot_line(out, 0)
    expect(b, "placed %d  missing 4 (log 0)" % (N - 4),
           "missing runs {1: 1, 3: 1}")
    expect(slot_line(out, 2), "missing 0")


@case
def dup_stale_retx(tmp):
    # The sender missed its stage window: the engine re-sends the previous
    # payload, so frame f carries chunk f-1 again and every later chunk is
    # one behind.
    def s0(f, log):
        log.rx(f, 0, payload(A, f - 1 if f >= 30 else f))
        return True
    b = slot_line(run(base(slot0=s0), [A, B], tmp), 0)
    expect(b, "dup 1  stale 0", "missing 0", "0 mismatched chunks",
           "at 1 boundaries")


@case
def stale(tmp):
    def s0(f, log):
        if f == 31:
            p = bytearray(payload(A, 30))
            p[20] ^= 0xFF
            log.rx(f, 0, bytes(p))
            return True
        return False
    b = slot_line(run(base(slot0=s0), [A, B], tmp), 0)
    expect(b, "stale 1", "missing 1")


@case
def out_of_order(tmp):
    def s0(f, log):
        if f in (50, 51):
            log.rx(f, 0, payload(A, 101 - f))	# 51 arrives before 50
            return True
        return False
    b = slot_line(run(base(slot0=s0), [A, B], tmp), 0)
    expect(b, "out-of-order 1", "missing 0", "0 mismatched chunks")


@case
def shifted_first_packet(tmp):
    # Card #23: SEC 2's first packet is a received packet written at TX
    # buffer offset 4: [4 B radio header] + [received payload 0..35]. Here
    # it carries the master's chunk 0.
    def s2(f, log):
        if f == 0:
            hdr = bytes([0x00, 0x00, 0x08, 0x00])	# ctr 8, slot 0
            log.rx(f, 2, hdr + payload(A, 0)[:36])
            return True
        return False
    b = slot_line(run(base(slot2=s2), [A, B], tmp), 2)
    expect(b, "foreign 1", "card #23 shifted first packet",
           "carries clip %04x chunk 0 (bytes match)" % (zlib.crc32(A) & 0xFFFF),
           "0 mismatched chunks")


@case
def rtt_seq_gap(tmp):
    # 10 records lost between frames 60 and 63 on the capture: a log gap.
    def s0(f, log):
        if 60 <= f < 63:
            return True
        if f == 63:
            log.rx(f, 0, payload(A, f), skip_seq=7)
            return True
        return False

    def s2(f, log):
        return 60 <= f < 63
    out = run(base(slot0=s0, slot2=s2), [A, B], tmp)
    expect(out, "missing from the sequence")
    expect(slot_line(out, 0), "missing 3 (log 3)")


@case
def corrupted(tmp):
    def s0(f, log):
        if f == 77:
            p = bytearray(payload(A, f))
            p[12] ^= 0x01
            p[30] ^= 0x80
            log.rx(f, 0, bytes(p))
            return True
        return False
    b = slot_line(run(base(slot0=s0), [A, B], tmp), 0)
    expect(b, "1 mismatched chunks, 2 byte errors", "first mismatched: [77]")


@case
def index_wrap(tmp):
    # A run past 65536 chunks (87 min): indices wrap and must stay placed.
    log = Log(1, 50)
    for f in range(65530, 65546):
        log.rx(f, 0, payload(A, f))
        log.rx(f, 2, payload(B, f))
    b = slot_line(run(log, [A, B], tmp), 0)
    expect(b, "placed 16  missing 0", "chunks 65530..65545",
           "0 mismatched chunks")


@case
def tx_pairing(tmp):
    peer = Log(0, 50)
    for f in range(N):
        peer.tx(payload(A, f))
    peer_path = os.path.join(tmp, "peer.bin")
    peer.write(peer_path)
    out = run(base(slot0=lambda f, log: f in (5, 6)), [A, B], tmp,
              ["--tx", peer_path])
    expect(slot_line(out, 0), "link 0->1", "sent %d, received %d, lost 2"
           % (N, N - 2), "latency: unavailable")


@case
def latency(tmp):
    # The master (slot 0) stages chunk f 30 ms before its slot boundary;
    # the receiver (slot 1) logs DIO1 AIR_US after the boundary. The two
    # clocks are unrelated and the master's runs 20 ppm fast; both hear
    # SEC 2 (slot 2), which is the common view.
    lead = 30000
    sender = Log(0, 50, clock_off=123_456_789, ppm=20.0)
    for f in range(N):
        boundary = 1_000_000 + f * FRAME_US
        sender.tx(payload(A, f), stage_true=boundary - lead)
        sender.rx(f, 2, payload(B, f))
    peer_path = os.path.join(tmp, "peer.bin")
    sender.write(peer_path)
    out = run(base(), [A, B], tmp, ["--tx", peer_path])
    want = (lead + AIR_US) / 1000.0
    expect(slot_line(out, 0), "latency 0->1",
           "min %.2f / median %.2f / max %.2f ms over %d chunks"
           % (want, want, want, N), "common view of slot 2")


def main():
    failed = 0
    for fn in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                fn(tmp)
                print("ok    %s" % fn.__name__)
            except AssertionError as e:
                failed += 1
                print("FAIL  %s\n%s" % (fn.__name__, e))
    print("%d/%d passed" % (len(CASES) - failed, len(CASES)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
