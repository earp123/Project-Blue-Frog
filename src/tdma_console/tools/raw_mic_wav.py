#!/usr/bin/env python3
"""Raw mic stream (.raw, "rawmic on") -> WAV, with an A/B against Codec 2.

In mic mode with "rawmic on", a HAT unit streams every 20 ms mono frame its
encoder is fed (rtt_link.h: "RM", frame k, 0, chunk n uint32, 160 int16), and
rtt_link.py capture saves them as <capture>.raw next to the soak file.

    raw_mic_wav.py S1_x.raw                  # -> S1_x_raw.wav
    raw_mic_wav.py S1_x.raw --log S1_x.bin   # also S1_x_ab.wav

Frames are placed by (chunk, frame), so a skipped frame (RTT buffer full)
is 20 ms of silence and the timeline stays aligned with the chunk index.
With --log (the same unit's soak file), the unit's own TX chunks are decoded
with the reference Codec 2 and written beside the raw audio as a stereo
file: left = raw mic, right = the same audio through Codec 2 3200, aligned
to the chunk. That is what every listener receives from this talker, minus
radio loss.
"""

import argparse
import struct
import sys
import wave

import numpy as np

import c2codec
from decode_soak_log import REC_TX, iter_records
from reconstruct_c2 import HDR, MODE, SAMPLES_PER_CHUNK, Unwrap, header
from reconstruct_tone import trusted_records, write_wav

FRAME = 160
REC = 8 + 2 * FRAME
FRAMES_PER_CHUNK = SAMPLES_PER_CHUNK // FRAME


def parse_raw(blob):
    """{(n, k): samples}, plus the count of bytes skipped resyncing."""
    frames, skipped, i = {}, 0, 0
    while i + REC <= len(blob):
        if blob[i:i + 2] != b"RM" or blob[i + 2] >= FRAMES_PER_CHUNK:
            i += 1
            skipped += 1
            continue
        k = blob[i + 2]
        (n,) = struct.unpack_from("<I", blob, i + 4)
        frames[(n, k)] = np.frombuffer(blob, "<i2", FRAME, i + 8)
        i += REC
    return frames, skipped


def tx_decoded(path):
    """{chunk index: 640 decoded samples} of the unit's own TX records."""
    with open(path, "rb") as f:
        recs = list(iter_records(f.read()))
    good, _ = trusted_records(recs)
    unwrap = Unwrap()
    chunks = {}
    for r in good:
        if r.type != REC_TX:
            continue
        h = header(r.payload)
        if h is None or h[1] != 0:
            continue
        chunks.setdefault(unwrap(h[0]), bytes(r.payload[HDR:HDR + 32]))
    keys = sorted(chunks)
    pcm = c2codec.decode(b"".join(chunks[u] for u in keys), MODE)
    return {u: pcm[i * SAMPLES_PER_CHUNK:(i + 1) * SAMPLES_PER_CHUNK]
            for i, u in enumerate(keys)}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw")
    ap.add_argument("--log", help="the same unit's soak file: also write "
                                  "an A/B stereo WAV (raw | Codec 2)")
    args = ap.parse_args()

    with open(args.raw, "rb") as f:
        frames, skipped = parse_raw(f.read())
    if not frames:
        sys.exit("no raw mic frames in %s" % args.raw)
    lo = min(n for n, _ in frames)
    hi = max(n for n, _ in frames)
    nch = hi - lo + 1
    raw = np.zeros(nch * SAMPLES_PER_CHUNK, np.int16)
    for (n, k), s in frames.items():
        off = (n - lo) * SAMPLES_PER_CHUNK + k * FRAME
        raw[off:off + FRAME] = s
    stem = args.raw.rsplit(".", 1)[0]
    write_wav(stem + "_raw.wav", raw, c2codec.FS)
    missing = nch * FRAMES_PER_CHUNK - len(frames)
    rms = np.sqrt(np.mean(raw.astype(float) ** 2))
    print("raw: chunks %d..%d, %d frames, %d missing, %d bytes resynced, "
          "peak %d, rms %.1f dBFS -> %s_raw.wav"
          % (lo, hi, len(frames), missing, skipped, int(np.abs(raw).max()),
             20 * np.log10(max(rms, 0.5) / 32768), stem))

    if args.log:
        dec = tx_decoded(args.log)
        coded = np.zeros_like(raw)
        for u, s in dec.items():
            if lo <= u <= hi:
                off = (u - lo) * SAMPLES_PER_CHUNK
                coded[off:off + len(s)] = s
        with wave.open(stem + "_ab.wav", "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(c2codec.FS)
            w.writeframes(np.stack([raw, coded], 1).astype("<i2").tobytes())
        write_wav(stem + "_coded.wav", coded, c2codec.FS)
        print("A/B: left raw, right Codec 2 (%d chunks decoded) -> %s_ab.wav, "
              "%s_coded.wav" % (sum(lo <= u <= hi for u in dec), stem, stem))


if __name__ == "__main__":
    main()
