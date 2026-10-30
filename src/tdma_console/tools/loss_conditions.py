#!/usr/bin/env python3
"""Received audio by loss condition: Opus as played vs Codec 2 under the same
losses, from clip soak logs (docs/opus_payload.md).

    loss_conditions.py LOG [LOG ...] --clips soaks/clips [-o DIR] [--win-s 10]

For every link in the logs (each receiver's view of each peer), the chunks
that arrived are placed by index (reconstruct_c2.py's analysis). Two streams
are then rebuilt with exactly that loss pattern:
    opus   the .op72 clip as a receiver plays it: PLC for up to two chunks,
           then silence (opuscodec.decode_chunks())
    c2     the same talker's .c2 clip, chunk for chunk (both are 80 ms per
           chunk, so chunk k is the same speech in both), as today's
           receiver plays it: one decoder skipping lost chunks, silence in
           their place
and each is scored per window with STOI against its own lossless decode,
so 1.000 is "the link cost nothing" and the drop is what the losses did to
that codec.

Windows are binned by loss: clean 0 %, light (0, 3) %, moderate [3, 10) %,
heavy [10, 30) %, severe >= 30 %. The table gives each bin's window count and
mean STOI for both codecs; for every non-empty bin, <bin>_ab.wav is the
window nearest the bin's median loss: Codec 2, 0.5 s of silence, then Opus,
the same speech and the same lost chunks. Writes loss_conditions.json too.
"""

import argparse
import json
import os
import statistics
import wave

import numpy as np
from pystoi import stoi

import c2codec
import opuscodec
from reconstruct_c2 import (CODEC_OPUS, Clip, analyse_slot, clean_rx,
                            frame_period, load, place)

FS = 8000
CHUNK_SAMPLES = 640
BINS = [("clean", 0.0, 0.0), ("light", 0.0, 0.03), ("moderate", 0.03, 0.10),
        ("heavy", 0.10, 0.30), ("severe", 0.30, 1.01)]


def bin_of(loss):
    if loss == 0:
        return "clean"
    for name, lo, hi in BINS[1:]:
        if lo <= loss < hi:
            return name
    return "severe"


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(np.asarray(pcm).clip(-32768, 32767).astype("<i2")
                      .tobytes())


def c2_stream(clip, lo, hi, got):
    """Codec 2 as today's receiver plays it: one decoder, lost chunks
    skipped and silent."""
    keep = [u for u in range(lo, hi + 1) if u in got]
    pcm = c2codec.decode(b"".join(clip.chunk(u) for u in keep), 3200)
    out = np.zeros((hi - lo + 1) * CHUNK_SAMPLES, np.int16)
    for i, u in enumerate(keep):
        out[(u - lo) * CHUNK_SAMPLES:(u - lo + 1) * CHUNK_SAMPLES] = \
            pcm[i * CHUNK_SAMPLES:(i + 1) * CHUNK_SAMPLES]
    return out


def links(paths, clips_dir):
    """(receiver slot, sender slot, opus Clip, c2 Clip, lo, hi, got) per link."""
    by_id = {}
    for f in sorted(os.listdir(clips_dir)):
        if f.endswith(".op72"):
            c = Clip(os.path.join(clips_dir, f))
            c2 = os.path.join(clips_dir, f[:-5] + ".c2")
            by_id[c.clip_id] = (c, Clip(c2) if os.path.exists(c2) else None)
    for path in paths:
        recs, meta = load(path)
        rx, _ = clean_rx(recs, meta["slot_id"], meta["slot_count"])
        chunks = place(rx, meta["slot_us"],
                       frame_period(rx, meta["slot_us"], meta["frame_us"]))
        peers = {}
        for c in chunks:
            peers.setdefault(c.slot, []).append(c)
        for slot in sorted(peers):
            res = analyse_slot(peers[slot], {k: v[0] for k, v in by_id.items()})
            if not res["placed"] or not res["clip_ids"]:
                continue
            cid = res["clip_ids"].most_common(1)[0][0]
            if cid not in by_id or by_id[cid][1] is None:
                continue
            op, c2 = by_id[cid]
            keys = sorted(res["placed"])
            yield (meta["slot_id"], slot, op, c2, keys[0], keys[-1],
                   set(keys))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--clips", default=os.path.join("soaks", "clips"))
    ap.add_argument("-o", "--out", default=None,
                    help="default: next to the first log")
    ap.add_argument("--win-s", type=float, default=10.0)
    args = ap.parse_args()
    out = args.out or os.path.dirname(os.path.abspath(args.logs[0]))
    os.makedirs(out, exist_ok=True)
    win = int(args.win_s * FS / CHUNK_SAMPLES)

    windows = []
    per_link = []
    for rcv, snd, op, c2, lo, hi, got in links(args.logs, args.clips):
        assert op.codec == CODEC_OPUS
        span = range(lo, hi + 1)
        lost = [u for u in span if u not in got]
        opus = opuscodec.decode_chunks([op.chunk(u) if u in got else None
                                        for u in span])
        opus_ref = opuscodec.decode_chunks([op.chunk(u) for u in span])
        c2s = c2_stream(c2, lo, hi, got)
        c2_ref = c2_stream(c2, lo, hi, set(span))
        longest = run = 0
        for u in span:
            run = run + 1 if u not in got else 0
            longest = max(longest, run)
        per_link.append({"rx": rcv, "tx": snd, "clip": op.name,
                         "chunks": len(span), "lost": len(lost),
                         "longest_gap_chunks": longest})
        for w0 in range(0, len(span) - win + 1, win):
            n_lost = sum(1 for u in span[w0:w0 + win] if u not in got)
            a, b = w0 * CHUNK_SAMPLES, (w0 + win) * CHUNK_SAMPLES
            if n_lost:
                s_op = stoi(opus_ref[a:b].astype(float),
                            opus[a:b].astype(float), FS)
                s_c2 = stoi(c2_ref[a:b].astype(float),
                            c2s[a:b].astype(float), FS)
            else:
                s_op = s_c2 = 1.0
            windows.append({"rx": rcv, "tx": snd, "clip": op.name,
                            "start_chunk": lo + w0, "loss": n_lost / win,
                            "bin": bin_of(n_lost / win), "stoi_opus": s_op,
                            "stoi_c2": s_c2, "_pcm": (c2s[a:b], opus[a:b])})

    print("links: " + ", ".join("%d->%d %d/%d lost (longest gap %d)" % (
        l["tx"], l["rx"], l["lost"], l["chunks"], l["longest_gap_chunks"])
        for l in per_link))
    print("\n%-9s %7s %9s %10s %10s   %s" % ("loss", "windows", "loss avg",
                                            "STOI c2", "STOI opus",
                                            "listening file"))
    summary = {}
    gap = np.zeros(FS // 2, np.int16)
    for name, _, _ in BINS:
        ws = [w for w in windows if w["bin"] == name]
        if not ws:
            print("%-9s %7d" % (name, 0))
            continue
        med = statistics.median(w["loss"] for w in ws)
        pick = min(ws, key=lambda w: abs(w["loss"] - med))
        path = os.path.join(out, "%s_ab.wav" % name)
        write_wav(path, np.concatenate([pick["_pcm"][0], gap, pick["_pcm"][1]]))
        summary[name] = {
            "windows": len(ws),
            "loss_mean": float(np.mean([w["loss"] for w in ws])),
            "stoi_c2": float(np.mean([w["stoi_c2"] for w in ws])),
            "stoi_opus": float(np.mean([w["stoi_opus"] for w in ws])),
            "example": {k: v for k, v in pick.items() if k != "_pcm"}}
        print("%-9s %7d %8.1f%% %10.3f %10.3f   %s (%d->%d, %.0f%% lost)" % (
            name, len(ws), 100 * summary[name]["loss_mean"],
            summary[name]["stoi_c2"], summary[name]["stoi_opus"],
            os.path.basename(path), pick["tx"], pick["rx"],
            100 * pick["loss"]))
    with open(os.path.join(out, "loss_conditions.json"), "w") as f:
        json.dump({"win_s": args.win_s, "links": per_link, "bins": summary,
                   "windows": [{k: v for k, v in w.items() if k != "_pcm"}
                               for w in windows]}, f, indent=1)


if __name__ == "__main__":
    main()
