#!/usr/bin/env python3
"""Prepare the speech clips for a Codec 2 transport soak.

Usage:
    c2clip.py fetch [-d soaks/clips]        # download the three OSR files
    c2clip.py encode in.wav -o clip.c2 [--mode 3200]

encode takes 8 kHz, 16-bit mono PCM (Codec 2's native input), encodes it,
trims to a whole number of 32-byte payload chunks (4 frames = 80 ms at 3200),
and writes clip.c2 plus clip_ref.wav, the clip's own decode: what a lossless
link should play back, and the reference reconstruct_c2.py scores against.
It prints the chunk count and the CRC32 that the unit reports after
`rtt_link.py clip clip.c2`.

Speech: the Open Speech Repository (Telchemy), Harvard sentences, free to use,
copy and publish. Credit: "Open Speech Repository". One file per unit, so each
stream is recognisable by ear and by clip_id (docs/rtt_link_c2_transport.md):

    MASTER  OSR_us_000_0010_8k.wav  female  -> OSR_0010_F.c2
    SEC 1   OSR_us_000_0030_8k.wav  male    -> OSR_0030_M.c2
    SEC 2   OSR_us_000_0011_8k.wav  female  -> OSR_0011_F.c2
"""

import argparse
import os
import sys
import urllib.request
import wave
import zlib

import numpy as np

import c2codec

CHUNK = 32	# clip bytes per payload (clip_src.h CLIP_CHUNK)
OSR_URL = "https://voiptroubleshooter.com/open_speech/american/"
OSR_FILES = ["OSR_us_000_0010_8k.wav", "OSR_us_000_0030_8k.wav",
             "OSR_us_000_0011_8k.wav"]


def read_wav(path):
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getsampwidth(), w.getnchannels()) != \
                (c2codec.FS, 2, 1):
            sys.exit("%s: need 8 kHz 16-bit mono, got %d Hz %d-bit %d ch" % (
                path, w.getframerate(), 8 * w.getsampwidth(),
                w.getnchannels()))
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(c2codec.FS)
        w.writeframes(np.asarray(pcm, dtype="<i2").tobytes())


def ref_path(clip_path):
    return os.path.splitext(clip_path)[0] + "_ref.wav"


def cmd_fetch(args):
    os.makedirs(args.dir, exist_ok=True)
    for name in OSR_FILES:
        dst = os.path.join(args.dir, name)
        if os.path.exists(dst):
            print("have", dst)
            continue
        req = urllib.request.Request(OSR_URL + name,
                                     headers={"User-Agent": "c2clip.py"})
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
        with open(dst, "wb") as f:
            f.write(data)
        print("got ", dst, len(data), "B")
    print('Speech: Open Speech Repository (credit "Open Speech Repository").')


def cmd_encode(args):
    if args.mode not in c2codec.CODEC_ENUM:
        sys.exit("mode %d has no clip-header codec value yet; this task "
                 "sends 3200 only" % args.mode)
    pcm = read_wav(args.wav)
    frames = c2codec.encode(pcm, args.mode)
    frames = frames[:len(frames) // CHUNK * CHUNK]
    if not frames:
        sys.exit("%s: shorter than one %d-byte chunk" % (args.wav, CHUNK))
    ref = c2codec.decode(frames, args.mode)

    with open(args.out, "wb") as f:
        f.write(frames)
    write_wav(ref_path(args.out), ref)

    chunks = len(frames) // CHUNK
    crc = zlib.crc32(frames) & 0xFFFFFFFF
    per = CHUNK // c2codec.frame_bytes(args.mode) * \
        c2codec.frame_samples(args.mode) / c2codec.FS
    print("%s: %d B, %d chunks (%.2f s at %.0f ms/chunk), crc32 %08x, "
          "clip_id %04x  [%s]" % (args.out, len(frames), chunks,
                                  chunks * per, per * 1000, crc,
                                  crc & 0xFFFF, c2codec.backend()))
    print("%s: reference decode" % ref_path(args.out))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fetch", help="download the three OSR speech files")
    p.add_argument("-d", "--dir", default=os.path.join("soaks", "clips"))
    p = sub.add_parser("encode", help="WAV -> .c2 clip + _ref.wav")
    p.add_argument("wav")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--mode", type=int, default=3200,
                   choices=sorted(c2codec.MODES))
    args = ap.parse_args()
    {"fetch": cmd_fetch, "encode": cmd_encode}[args.cmd](args)


if __name__ == "__main__":
    main()
