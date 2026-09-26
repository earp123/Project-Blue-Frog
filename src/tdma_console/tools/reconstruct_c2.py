#!/usr/bin/env python3
"""Place, verify, decode and score the Codec 2 streams in a clip soak log.

A clip soak (payload mode 2, docs/rtt_link_c2_transport.md) sends 40-byte
payloads of an 8-byte test header plus 32 bytes of a pre-encoded Codec 2 3200
clip: four 20 ms frames, 80 ms of speech per packet. Every RX record in a
unit's log (SD file or rtt_link.py capture) holds one peer chunk.

Usage:
    reconstruct_c2.py capture.bin --clip OSR_0010_F.c2 --clip OSR_0030_M.c2 \\
        --clip OSR_0011_F.c2 [-o out_dir] [--tx peer.bin ...] [--json out.json]

Per peer slot:
  1. Header check (magic 0xC2, codec 0 = 3200). A bad header is "foreign";
     card #23's first packet (a received packet written at TX buffer offset
     4) is recognised by the shifted header and named as such.
  2. Placement by chunk index, unwrapped from 16 bits. Each chunk is placed,
     a dup (same index, same bytes), stale (same index, different bytes:
     should never happen) or out-of-order (an index below one already seen).
     Placement is cross-checked against arrival time (t_us), as
     reconstruct_tone.py places chunks, and disagreements are reported.
  3. Integrity: the clip_id picks the supplied clip, and every placed chunk's
     32 bytes are compared with the clip at (index mod chunks) * 32.
  4. Delivery: placed / missing / dup / stale / out-of-order / foreign, and a
     histogram of missing-run lengths. A gap across a log break (SD hole or
     ring/RTT drop) is blamed on the log, not the radio.
  5. Audio: each stream through one persistent Codec 2 decoder; a missing
     chunk is 80 ms of silence (no concealment). Writes <stem>_slot<N>.wav
     and, if the pesq module is importable, scores PESQ (nb) against the
     clip's _ref.wav (c2clip.py) aligned by the first placed index.
  6. With --tx (the peers' own logs): pairs each peer's TX records with this
     unit's RX records by (clip_id, chunk index) for per-link delivery, sent
     vs received, and latency: RX DIO1 time minus the peer's TX stage time
     (SOAK_F_TX_T records). The two units' slot clocks are free-running and
     unrelated, so the offset between them is measured by common view: both
     units hear a third unit's chunk k, and the difference of their two RX
     times is the clock offset at that moment (nearest sample, so crystal
     drift over the run cancels). Needs three units. The figure is
     stage-to-DIO1 on the air path: it excludes encode, decode and playout.

Codec 2 comes from c2codec.py (pycodec2, else c2enc/c2dec on PATH).
"""

import argparse
import bisect
import json
import os
import struct
import sys
import zlib
from collections import Counter

import numpy as np

import c2codec
from decode_soak_log import (PAYLOAD_CLIP, REC_META, REC_TX, iter_records,
                             parse_meta, payload_desc)
from reconstruct_tone import (clean_rx, frame_period, place, trusted_records,
                              write_wav)

F_TX_T = 1 << 1		# soak_log.h SOAK_F_TX_T: TX t_us is the stage time
OFFSET_MAX_GAP_US = 2_000_000	# farthest common-view sample to trust
MAGIC = 0xC2
HDR = 8
CHUNK = 32
MODE = 3200
FRAMES_PER_CHUNK = CHUNK // c2codec.frame_bytes(MODE)			# 4
SAMPLES_PER_CHUNK = FRAMES_PER_CHUNK * c2codec.frame_samples(MODE)	# 640

try:
    from pesq import pesq as pesq_score
except ImportError:
    pesq_score = None


def s16(x):
    return ((x + 0x8000) & 0xFFFF) - 0x8000


class Unwrap:
    """16-bit chunk index -> monotonic index, relative to the highest seen."""

    def __init__(self):
        self.top = None

    def __call__(self, idx16):
        if self.top is None:
            u = idx16
        else:
            u = self.top + s16(idx16 - (self.top & 0xFFFF))
        if self.top is None or u > self.top:
            self.top = u
        return u


def header(p):
    """(chunk_idx, clip_id) of a valid clip payload, else None."""
    if p[0] != MAGIC or p[1] != c2codec.CODEC_ENUM[MODE]:
        return None
    idx, cid = struct.unpack_from("<HH", p, 2)
    return idx, cid


def shifted_header(p):
    """Card #23: [4 B received radio header] + [received payload 0..35].

    Returns (chunk_idx, clip_id) of the clip chunk it carries, or None.
    """
    if p[4] != MAGIC or p[5] != c2codec.CODEC_ENUM[MODE]:
        return None
    return struct.unpack_from("<HH", p, 6)


class Clip:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if not self.data or len(self.data) % CHUNK:
            sys.exit("%s: not a clip (%d B is not a multiple of %d)"
                     % (path, len(self.data), CHUNK))
        self.path = path
        self.name = os.path.basename(path)
        self.chunks = len(self.data) // CHUNK
        self.crc = zlib.crc32(self.data) & 0xFFFFFFFF
        self.clip_id = self.crc & 0xFFFF

    def chunk(self, n):
        off = (n % self.chunks) * CHUNK
        return self.data[off:off + CHUNK]

    def ref_pcm(self):
        """The clip's own decode (c2clip.py writes it), or decode it now."""
        ref = os.path.splitext(self.path)[0] + "_ref.wav"
        if os.path.exists(ref):
            import wave
            with wave.open(ref, "rb") as w:
                return np.frombuffer(w.readframes(w.getnframes()), "<i2")
        return c2codec.decode(self.data, MODE)


def load(path):
    with open(path, "rb") as f:
        recs = list(iter_records(f.read()))
    meta = next((parse_meta(r.payload) for r in recs if r.type == REC_META),
                None)
    if meta is None:
        sys.exit("%s: no valid META record" % path)
    if meta["payload_mode"] != PAYLOAD_CLIP:
        sys.exit("%s: not a clip log (%s)" % (path, payload_desc(meta)))
    return recs, meta


def runs_hist(missing):
    """Missing indices -> {run length: count}."""
    hist = Counter()
    run = 0
    prev = None
    for m in sorted(missing):
        if prev is not None and m == prev + 1:
            run += 1
        else:
            if run:
                hist[run] += 1
            run = 1
        prev = m
    if run:
        hist[run] += 1
    return dict(sorted(hist.items()))


def analyse_slot(chunks, clips):
    """One peer's chunks in arrival order -> stats and placed payloads."""
    st = Counter()
    placed = {}		# unwrapped index -> (32 B, brk)
    frame_of = {}	# unwrapped index -> arrival frame
    unwrap = Unwrap()
    clip_ids = Counter()
    foreign = []
    mismatched = []
    byte_errors = 0
    disagree = []
    prev = None		# (index, frame) of the last in-order placed chunk
    top = None

    for c in chunks:
        p = bytes(c.payload)
        h = header(p)
        if h is None:
            st["foreign"] += 1
            sh = shifted_header(p)
            if sh is not None:
                clip = clips.get(sh[1])
                ok = (clip is not None and
                      p[4 + HDR:] == clip.chunk(sh[0])[:len(p) - 4 - HDR])
                foreign.append("frame %d: card #23 shifted first packet "
                               "(+4 B), carries clip %04x chunk %d%s"
                               % (c.frame, sh[1], sh[0],
                                  " (bytes match)" if ok else ""))
            else:
                foreign.append("frame %d: bad header %s"
                               % (c.frame, p[:8].hex()))
            continue

        idx16, cid = h
        clip_ids[cid] += 1
        u = unwrap(idx16)
        body = p[HDR:]

        if u in placed:
            st["dup" if placed[u][0] == body else "stale"] += 1
            continue

        placed[u] = (body, c.brk)
        frame_of[u] = c.frame
        if top is not None and u < top:
            st["out_of_order"] += 1
        else:
            st["placed"] += 1
            # Arrival-time cross-check: frames elapsed should equal chunks.
            if prev is not None and (u - prev[0]) != (c.frame - prev[1]):
                disagree.append((prev[0], u, c.frame - prev[1]))
            prev = (u, c.frame)
            top = u

        if p[6:8] != b"\0\0":
            st["rsvd_nonzero"] += 1
        clip = clips.get(cid)
        if clip is None:
            st["unknown_clip"] += 1
            continue
        want = clip.chunk(u)
        if body != want:
            mismatched.append(u)
            byte_errors += sum(a != b for a, b in zip(body, want))

    missing, missing_log = [], 0
    if placed:
        lo, hi = min(placed), max(placed)
        missing = [u for u in range(lo, hi + 1) if u not in placed]
        # A gap is the log's if the chunks either side of it straddle a
        # log break.
        keys = sorted(placed)
        for a, b in zip(keys, keys[1:]):
            if b - a > 1 and placed[a][1] != placed[b][1]:
                missing_log += b - a - 1

    return {
        "stats": st, "placed": placed, "missing": missing,
        "missing_log": missing_log, "clip_ids": clip_ids, "foreign": foreign,
        "mismatched": mismatched, "byte_errors": byte_errors,
        "disagree": disagree,
    }


def audio(res, clip, out_path):
    """Decode the stream, silence for gaps; returns (samples, pesq or None)."""
    placed = res["placed"]
    keys = sorted(placed)
    lo, hi = keys[0], keys[-1]
    present = b"".join(placed[u][0] for u in keys)
    pcm = c2codec.decode(present, MODE)
    out = np.zeros((hi - lo + 1) * SAMPLES_PER_CHUNK, np.int16)
    for i, u in enumerate(keys):
        seg = pcm[i * SAMPLES_PER_CHUNK:(i + 1) * SAMPLES_PER_CHUNK]
        out[(u - lo) * SAMPLES_PER_CHUNK:][:len(seg)] = seg
    write_wav(out_path, out, c2codec.FS)

    score = None
    if pesq_score is not None and clip is not None:
        ref = clip.ref_pcm().astype(np.int16)
        start = (lo % clip.chunks) * SAMPLES_PER_CHUNK
        reps = (start + len(out)) // len(ref) + 1
        ref = np.tile(ref, reps)[start:start + len(out)]
        try:
            score = float(pesq_score(c2codec.FS, ref, out, "nb"))
        except Exception as e:	# silence-only or too short
            score = "n/a (%s)" % e
    return len(out), score


def s32(x):
    return ((x + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


def rx_times(recs, meta):
    """{peer slot: {(clip_id, unwrapped index): first RX t_us}}."""
    rx, _ = clean_rx(recs, meta["slot_id"], meta["slot_count"])
    times = {}
    unwraps = {}
    for r, _brk in rx:
        h = header(r.payload)
        if h is None:
            continue
        idx16, cid = h
        u = unwraps.setdefault(r.slot_id, Unwrap())(idx16)
        times.setdefault(r.slot_id, {}).setdefault((cid, u), r.t_us)
    return times


def tx_sent(path):
    """A peer's own log -> (slot, {clip_id: {index: stage t_us or None}},
    its RX times)."""
    recs, meta = load(path)
    good, _ = trusted_records(recs)
    sent = {}
    unwraps = {}
    for r in good:
        if r.type != REC_TX:
            continue
        h = header(r.payload)
        if h is None:
            continue
        idx16, cid = h
        u = unwraps.setdefault(cid, Unwrap())(idx16)
        sent.setdefault(cid, {})[u] = r.t_us if r.flags & F_TX_T else None
    return meta["slot_id"], sent, rx_times(recs, meta)


def latency(sender, receiver, stage, rx_rcv, rx_snd):
    """Stage-to-DIO1 latencies (us) for the sender -> receiver link.

    stage: {(cid, u): stage t_us} in the sender's clock; rx_rcv / rx_snd:
    rx_times() of the receiver's and the sender's logs. Returns (latencies,
    common-view samples used, third slots used).
    """
    # Unwrap the sender's clock around its first stage time: the counter
    # wraps every 71.6 min, a run is minutes long.
    if not stage:
        return [], 0, []
    ref = next(iter(stage.values()))

    def unw(t):
        return ref + s32(t - ref)

    samples = []	# (sender time, receiver - sender offset)
    thirds = []
    for x in rx_rcv:
        if x in (sender, receiver) or x not in rx_snd:
            continue
        common = rx_rcv[x].keys() & rx_snd[x].keys()
        if common:
            thirds.append(x)
        for k in common:
            ts = rx_snd[x][k]
            samples.append((unw(ts), s32(rx_rcv[x][k] - ts)))
    if not samples:
        return [], 0, []
    samples.sort()
    keys = [s[0] for s in samples]

    lat = []
    got = rx_rcv.get(sender, {})
    for k, ts in stage.items():
        if k not in got:
            continue
        t = unw(ts)
        i = bisect.bisect_left(keys, t)
        near = min((j for j in (i - 1, i) if 0 <= j < len(keys)),
                   key=lambda j: abs(keys[j] - t))
        if abs(keys[near] - t) > OFFSET_MAX_GAP_US:
            continue
        lat.append(s32(got[k] - (ts + samples[near][1])))
    return lat, len(samples), sorted(thirds)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile")
    ap.add_argument("--clip", action="append", default=[], required=True,
                    help="a clip the units sent (repeat for each)")
    ap.add_argument("-o", "--out-dir",
                    help="where the WAVs go (default: next to the log)")
    ap.add_argument("--tx", action="append", default=[],
                    help="a peer's own log, for per-link pairing")
    ap.add_argument("--json", help="also write the summary as JSON here")
    args = ap.parse_args()

    clips = {}
    for p in args.clip:
        c = Clip(p)
        if c.clip_id in clips:
            sys.exit("%s and %s share clip_id %04x"
                     % (clips[c.clip_id].name, c.name, c.clip_id))
        clips[c.clip_id] = c

    recs, meta = load(args.logfile)
    own = meta["slot_id"]
    rx, tally = clean_rx(recs, own, meta["slot_count"])
    period = frame_period(rx, meta["slot_us"], meta["frame_us"])
    chunks = place(rx, meta["slot_us"], period)
    own_rx = rx_times(recs, meta)

    stem = os.path.splitext(os.path.basename(args.logfile))[0]
    out_dir = args.out_dir or os.path.dirname(os.path.abspath(args.logfile))
    os.makedirs(out_dir, exist_ok=True)

    peers = {}
    for c in chunks:
        peers.setdefault(c.slot, []).append(c)

    sent_by_slot = {}
    for p in args.tx:
        slot, sent, their_rx = tx_sent(p)
        sent_by_slot[slot] = (p, sent, their_rx)

    print("file:     %s" % args.logfile)
    print("logger:   %s slot=%d %s" % (meta["role"], own, payload_desc(meta)))
    print("hygiene:  %d records discarded (SD hole), %d missing from the "
          "sequence (hole + ring/RTT drops), %d own-slot RX ignored"
          % (tally["discarded"], tally["missing"], tally["own_slot"]))
    print("clips:    %s" % ", ".join("%s id %04x (%d chunks)"
                                     % (c.name, i, c.chunks)
                                     for i, c in sorted(clips.items())))

    summary = {"file": args.logfile, "slot": own, "role": meta["role"],
               "hygiene": tally, "peers": {}}
    for slot in sorted(peers):
        res = analyse_slot(peers[slot], clips)
        st = res["stats"]
        cids = res["clip_ids"]
        cid = cids.most_common(1)[0][0] if cids else None
        clip = clips.get(cid)
        name = clip.name if clip else ("clip %04x (not supplied)" % cid
                                       if cid is not None else "none")
        n_missing = len(res["missing"])
        print("slot %d    %s" % (slot, name))
        if len(cids) > 1:
            print("           clip_ids seen: %s" % ", ".join(
                "%04x x%d" % kv for kv in cids.most_common()))
        print("           placed %d  missing %d (log %d)  dup %d  stale %d  "
              "out-of-order %d  foreign %d" % (
                  st["placed"], n_missing, res["missing_log"], st["dup"],
                  st["stale"], st["out_of_order"], st["foreign"]))
        if res["placed"]:
            keys = sorted(res["placed"])
            print("           chunks %d..%d; missing runs %s" % (
                keys[0], keys[-1], runs_hist(res["missing"]) or "none"))
        for f in res["foreign"]:
            print("           foreign: %s" % f)
        if clip is not None:
            print("           integrity: %d mismatched chunks, %d byte errors "
                  "vs %s" % (len(res["mismatched"]), res["byte_errors"],
                             clip.name))
            if res["mismatched"]:
                print("             first mismatched: %s"
                      % res["mismatched"][:10])
        if st["unknown_clip"]:
            print("           integrity: %d chunks carry an unknown clip_id"
                  % st["unknown_clip"])
        if st["rsvd_nonzero"]:
            print("           header: %d chunks with rsvd != 0"
                  % st["rsvd_nonzero"])
        print("           arrival time disagrees with chunk index at %d "
              "boundaries" % len(res["disagree"]))
        for a, b, df in res["disagree"][:5]:
            print("             chunk %d -> %d over %d frame(s)" % (a, b, df))

        peer = {"clip": name, "placed": st["placed"], "missing": n_missing,
                "missing_log": res["missing_log"], "dup": st["dup"],
                "stale": st["stale"], "out_of_order": st["out_of_order"],
                "foreign": st["foreign"], "foreign_detail": res["foreign"],
                "missing_runs": runs_hist(res["missing"]),
                "mismatched": len(res["mismatched"]),
                "byte_errors": res["byte_errors"],
                "time_disagree": len(res["disagree"])}

        if res["placed"]:
            wav = os.path.join(out_dir, "%s_slot%d.wav" % (stem, slot))
            n, score = audio(res, clip, wav)
            print("           audio: %s (%.2f s)%s" % (
                wav, n / c2codec.FS,
                "" if score is None else ", PESQ nb %s" % (
                    "%.2f" % score if isinstance(score, float) else score)))
            peer["wav"] = wav
            peer["pesq_nb"] = score

        if slot in sent_by_slot and cid is not None:
            path, sent, their_rx = sent_by_slot[slot]
            tx = sent.get(cid, {})
            got = set(res["placed"])
            both = [u for u in tx if u in got]
            if tx:
                lo, hi = max(min(tx), min(got)), min(max(tx), max(got))
                in_range = [u for u in tx if lo <= u <= hi]
                lost = [u for u in in_range if u not in got]
                print("           link %d->%d (%s): sent %d, received %d, "
                      "lost %d over the common span %d..%d"
                      % (slot, own, os.path.basename(path), len(in_range),
                         len(in_range) - len(lost), len(lost), lo, hi))
                peer["link"] = {"sent": len(in_range),
                                "lost": len(lost), "span": [lo, hi]}
            stage = {(cid, u): tx[u] for u in both if tx[u] is not None}
            if both and not stage:
                print("           latency: unavailable. %s has no TX stage "
                      "times (firmware before SOAK_F_TX_T)"
                      % os.path.basename(path))
            elif stage:
                lat, n_off, thirds = latency(slot, own, stage, own_rx,
                                             their_rx)
                if not lat:
                    print("           latency: unavailable. No common view: "
                          "the two logs share no third unit's chunks")
                else:
                    a = np.array(lat) / 1000.0
                    print("           latency %d->%d, stage to DIO1 (air "
                          "path only; excludes encode, decode and playout): "
                          "min %.2f / median %.2f / max %.2f ms over %d "
                          "chunks; clock offset by common view of slot %s "
                          "(%d samples)" % (
                              slot, own, a.min(), float(np.median(a)),
                              a.max(), len(a),
                              "/".join(map(str, thirds)), n_off))
                    peer["latency_ms"] = {
                        "min": float(a.min()), "median": float(np.median(a)),
                        "max": float(a.max()), "n": len(a),
                        "common_view": thirds}
        summary["peers"][slot] = peer

    if pesq_score is None:
        print("pesq:     not installed; PESQ skipped")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(summary, f, indent=1, default=str)


if __name__ == "__main__":
    main()
