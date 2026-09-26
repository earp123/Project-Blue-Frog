#!/usr/bin/env python3
"""Score an on-device Codec 2 encode run (payload mode PCM).

Each unit encodes its own 8 kHz PCM clip on the device (c2_enc.c) and sends
the frames behind the clip test header; every TX record logs the chunk, the
encode time and the staging lead. Given every unit's capture of one run and
the PCM clips, this reports per unit:

  - encode time per 80 ms chunk (wall clock on the device, preemption by the
    radio and data threads included): min / median / p99 / max;
  - staging lead against the engine's pickup deadline (TDMA_PRE_TX_PICKUP_US
    less its ~50 us wake), and how many frames went out stale;
  - correctness: the device's frames against pycodec2 encoding the same PCM
    the way the device does (one fresh Codec 2 state per run, chunk 0, 1,
    2 ... of the looping clip). Bit-exact is expected apart from rare
    float-rounding flips between the M33 and the host's maths library;
  - over the air: each peer's received chunks against what that peer logged
    as sent, byte for byte.

Usage:
    c2_encode_check.py soaks/rtt/enc/*.bin --pcm soaks/clips/OSR_0010_F.pcm \\
        --pcm soaks/clips/OSR_0030_M.pcm --pcm soaks/clips/OSR_0011_F.pcm \\
        [--phase-us 70000] [--wav-dir out/]

See docs/c2_encode_test.md.
"""

import argparse
import os
import struct
import sys
import zlib

import numpy as np

import c2codec
from decode_soak_log import (PAYLOAD_PCM, REC_META, REC_RX, REC_STATS,
                             REC_TX, iter_records, parse_meta)
from reconstruct_c2 import Unwrap, header, trusted_records

F_TX_LEAD = 1 << 2
F_TX_ENC = 1 << 3
CHUNK_PCM = 640
DEADLINE_US = 2450	# measured pre-TX pickup deadline (docs/stage_lead_sweep.md)


def pcts(a):
    a = np.asarray(a, dtype=float)
    return "min %.2f / med %.2f / p99 %.2f / max %.2f ms" % (
        a.min() / 1000, np.median(a) / 1000, np.percentile(a, 99) / 1000,
        a.max() / 1000)


class Unit:
    def __init__(self, path):
        self.path = path
        recs = list(iter_records(open(path, "rb").read()))
        self.meta = next((parse_meta(r.payload) for r in recs
                          if r.type == REC_META), None)
        if self.meta is None or self.meta["payload_mode"] != PAYLOAD_PCM:
            sys.exit("%s: not a PCM (on-device encode) log" % path)
        self.slot = self.meta["slot_id"]
        good, _ = trusted_records(recs)
        self.tx = {}		# unwrapped n -> (payload, enc_us, lead_us)
        self.rx = {}		# peer slot -> {n: payload}
        self.clip_id = None
        uw = Unwrap()
        rx_uw = {}
        stats = []
        for r in good:
            if r.type == REC_STATS:
                stats.append(struct.unpack_from("<7I", r.payload))
            h = header(r.payload)
            if h is None:
                continue
            if r.type == REC_TX:
                n = uw(h[0])
                self.clip_id = h[1]
                enc = (r.rsvd[0] | r.rsvd[1] << 8 | r.rsvd[2] << 16
                       if r.flags & F_TX_ENC else None)
                lead = r.frame_ctr if r.flags & F_TX_LEAD else None
                self.tx.setdefault(n, (bytes(r.payload), enc, lead))
            elif r.type == REC_RX:
                n = rx_uw.setdefault(r.slot_id, Unwrap())(h[0])
                self.rx.setdefault(r.slot_id, {}).setdefault(
                    n, bytes(r.payload))
        self.tx_done = stats[-1][0] - stats[0][0] if len(stats) > 1 else None
        self.stale = stats[-1][5] - stats[0][5] if len(stats) > 1 else None


def host_stream(pcm, n_max):
    """What the device's encoder produces: chunk n for n = 0..n_max, one
    fresh state, the clip looping."""
    chunks = len(pcm) // CHUNK_PCM
    idx = np.arange((n_max + 1) * CHUNK_PCM) % (chunks * CHUNK_PCM)
    frames = c2codec.encode(pcm[idx], 3200)
    return [frames[i * 32:(i + 1) * 32] for i in range(n_max + 1)]


def bits_differ(a, b):
    return sum(bin(x ^ y).count("1") for x, y in zip(a, b))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--pcm", action="append", required=True,
                    help="a PCM clip one of the units encoded")
    ap.add_argument("--phase-us", type=int,
                    help="the run's phase setting, for the latency line")
    ap.add_argument("--wav-dir", help="write each unit's device-encoded "
                    "stream, decoded, as a WAV here")
    args = ap.parse_args()

    clips = {}
    for p in args.pcm:
        data = open(p, "rb").read()
        clips[zlib.crc32(data) & 0xFFFF] = (os.path.basename(p),
                                            np.frombuffer(data, "<i2"))
    units = [Unit(p) for p in args.captures]
    by_slot = {u.slot: u for u in units}

    for u in units:
        print("slot %d  %s" % (u.slot, os.path.basename(u.path)))
        sent = sorted(u.tx)
        if not sent:
            print("  nothing sent")
            continue
        enc = [u.tx[n][1] for n in sent if u.tx[n][1] is not None]
        lead = [u.tx[n][2] for n in sent if u.tx[n][2] is not None]
        print("  sent %d chunks (index %d..%d); engine tx_done %s, of them "
              "stale re-sends %s" % (len(sent), sent[0], sent[-1],
                                     u.tx_done, u.stale))
        if enc:
            print("  encode time per chunk: %s" % pcts(enc))
        if lead:
            short = sum(1 for x in lead if x < DEADLINE_US)
            print("  staging lead: %s; %d below the %.2f ms deadline" % (
                pcts(lead), short, DEADLINE_US / 1000))

        clip = clips.get(u.clip_id)
        if clip is None:
            print("  clip_id %s: no matching --pcm" % (
                "%04x" % u.clip_id if u.clip_id is not None else "?"))
        else:
            ref = host_stream(clip[1], sent[-1])
            exact = [n for n in sent if u.tx[n][0][8:] == ref[n]]
            bad = [n for n in sent if u.tx[n][0][8:] != ref[n]]
            nbits = sum(bits_differ(u.tx[n][0][8:], ref[n]) for n in bad)
            print("  vs pycodec2 on %s: %d / %d chunks bit-exact; %d differ "
                  "(%d of %d bits)" % (clip[0], len(exact), len(sent),
                                       len(bad), nbits, len(sent) * 256))
            if bad:
                print("    first differing chunks: %s" % bad[:10])
            if args.wav_dir:
                os.makedirs(args.wav_dir, exist_ok=True)
                import wave
                for tag, frames in (("device", b"".join(u.tx[n][0][8:]
                                                        for n in sent)),
                                    ("host", b"".join(ref[n] for n in sent))):
                    pcm = c2codec.decode(frames, 3200)
                    path = os.path.join(args.wav_dir, "slot%d_%s.wav" %
                                        (u.slot, tag))
                    with wave.open(path, "wb") as w:
                        w.setnchannels(1)
                        w.setsampwidth(2)
                        w.setframerate(8000)
                        w.writeframes(pcm.astype("<i2").tobytes())
                print("    WAVs: slot%d_device.wav, slot%d_host.wav" % (
                    u.slot, u.slot))

        for r in units:
            if r is u or u.slot not in r.rx:
                continue
            got = r.rx[u.slot]
            both = [n for n in got if n in u.tx]
            diff = [n for n in both if got[n] != u.tx[n][0]]
            print("  over the air to slot %d: %d received of %d sent in "
                  "span, %d differ from what was sent" % (
                      r.slot, len(both),
                      sum(1 for n in sent if min(got) <= n <= max(got)),
                      len(diff)))

        if args.phase_us and enc:
            print("  latency, first sample of a chunk to peer DIO1: 80 ms "
                  "(chunk) + %.1f ms (phase) + ~8.2 ms (air) = %.1f ms, "
                  "before decode and playout" % (
                      args.phase_us / 1000, 80 + args.phase_us / 1000 + 8.2))


if __name__ == "__main__":
    main()
