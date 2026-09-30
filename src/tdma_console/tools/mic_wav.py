#!/usr/bin/env python3
"""Decode a live-mic stream (payload mode MIC) from a soak log to a WAV.

In a mic soak (docs/live_audio_wm8960.md, G2) the talker's unit encodes its
WM8960 HAT's mics on the device and sends the same 8-byte test header plus
four Codec 2 3200 frames as the clip modes, with clip_id 0. There is no
reference clip, so this places the chunks by index and decodes them, and
the check is listening: the WAV should be the talker's speech.

    mic_wav.py peer.bin --slot 0 [-o talker_at_peer.wav]   # as received
    mic_wav.py talker.bin [-o talker_sent.wav]             # as staged (TX)

With --slot, the RX records from that sender in a receiver's log. Without
it, the log's own TX records: what the talker staged, for comparison.
Missing chunks are 80 ms of silence. Prints placed / missing / dup / foreign.
"""

import argparse
import sys

import numpy as np

import c2codec
from decode_soak_log import (PAYLOAD_MIC, REC_META, REC_RX, REC_TX,
                             iter_records, parse_meta)
from reconstruct_c2 import (FRAMES_PER_CHUNK, HDR, MODE, SAMPLES_PER_CHUNK,
                            Unwrap, header)
from reconstruct_tone import trusted_records, write_wav

CHUNK = FRAMES_PER_CHUNK * c2codec.frame_bytes(MODE)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--slot", type=int,
                    help="sender slot (RX records); default: own TX records")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    with open(args.log, "rb") as f:
        recs = list(iter_records(f.read()))
    meta = next((parse_meta(r.payload) for r in recs if r.type == REC_META),
                None)
    if meta is None:
        sys.exit("%s: no valid META record" % args.log)
    if args.slot is None and meta["payload_mode"] != PAYLOAD_MIC:
        print("note: this unit's own mode is %d, not mic (4)"
              % meta["payload_mode"])

    good, _ = trusted_records(recs)
    want = REC_TX if args.slot is None else REC_RX
    unwrap = Unwrap()
    placed, dup, foreign, other_id = {}, 0, 0, 0
    for r in good:
        if r.type != want or (args.slot is not None and r.slot_id != args.slot):
            continue
        h = header(r.payload)
        if h is None:
            foreign += 1
            continue
        idx16, cid = h
        if cid != 0:
            other_id += 1	# a clip, not live audio
            continue
        u = unwrap(idx16)
        if u in placed:
            dup += 1
            continue
        placed[u] = bytes(r.payload[HDR:HDR + CHUNK])

    if not placed:
        sys.exit("no mic chunks (clip_id 0) found")
    keys = sorted(placed)
    lo, hi = keys[0], keys[-1]
    missing = (hi - lo + 1) - len(keys)
    pcm = c2codec.decode(b"".join(placed[u] for u in keys), MODE)
    out = np.zeros((hi - lo + 1) * SAMPLES_PER_CHUNK, np.int16)
    for i, u in enumerate(keys):
        seg = pcm[i * SAMPLES_PER_CHUNK:(i + 1) * SAMPLES_PER_CHUNK]
        out[(u - lo) * SAMPLES_PER_CHUNK:][:len(seg)] = seg
    path = args.out or "%s_%s.wav" % (
        args.log.rsplit(".", 1)[0],
        "tx" if args.slot is None else "slot%d" % args.slot)
    write_wav(path, out, c2codec.FS)

    rms = np.sqrt(np.mean(out.astype(float) ** 2))
    print("%s: chunks %d..%d placed %d missing %d dup %d foreign %d "
          "clip-id!=0 %d  %.1f s  rms %.1f dBFS -> %s"
          % ("TX" if args.slot is None else "slot %d" % args.slot, lo, hi,
             len(keys), missing, dup, foreign, other_id,
             len(out) / c2codec.FS, 20 * np.log10(max(rms, 0.5) / 32768),
             path))


if __name__ == "__main__":
    main()
