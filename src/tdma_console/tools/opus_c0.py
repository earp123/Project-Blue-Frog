#!/usr/bin/env python3
"""Opus vs Codec 2, gate C0 of docs/opus_payload.md: is Opus worth a bigger
packet? Host only, before any firmware.

    opus_c0.py [-o soaks/opus_c0] [--seeds 10]

Encodes speech with Codec 2 3200 and with Opus at decision 2's settings,
decodes it as a receiver would, and scores STOI and ESTOI (pystoi) against
the clean speech: lossless, and with whole 80 ms chunks lost at random.

Material: the three OSR clips clip_src sends (soaks/clips, `c2clip.py
fetch`), clean speech scored against itself; and the D1 conditions
(`mic_dsp_ab.py make`, soaks/mic_dsp_d1: quiet, soft, crowd, wind, roar),
scored against the speech under the mix. A D1 clip is 8 s and loops on the
unit, so it is coded three times over and the middle copy scored: no
encoder start-up in the score. The DSP front end is not in this: it sits in
front of either codec.

Codecs, per 80 ms chunk:
    c2 3200     4 x 8 B Codec 2, what is sent today (32 B)
    opus 7.2k   4 x 18 B, decision 2 at complexity 2 (72 B: the 84 B packet)
    opus 5.6k   4 x 14 B, the 68 B fallback of decision 9, SILK forced
    5.6k auto   the same with the mode left to libopus: under 15 B a 20 ms
                frame is CELT (opus_encoder.c: "less than 6 kb/s, switch to
                CELT-only mode", in the OPUS_AUTO branch), what the fallback
                would be without OPUS_SET_FORCE_MODE
Loss: each chunk is lost with probability p (2, 5, 10 %), one pattern per
clip, rate and seed, shared by every codec. A lost Codec 2 chunk is
silence and its decoder skips it, as c2_dec.c and hat_audio_play_chunk() do
today. A lost Opus chunk runs the decoder's PLC for its four frames, played
for up to two chunks in a row and silence after (decision 3); "nPLC" plays
silence for every lost chunk instead, to show what the PLC buys.

Each codec's delay is found once per clip on its lossless decode (envelope
correlation, then the best STOI within +-12 samples) and used for its lossy
decodes too. Writes scores.json, and per D1 condition <name>_ab.wav (Codec 2,
then Opus 7.2k, 0.5 s apart) and <name>_loss_ab.wav (the same at 5 % loss,
one pattern) to listen to.

Pass (C0): Opus 7.2k >= +0.08 STOI over Codec 2 on clean speech, and the
engineer hears a clear step up.
"""

import argparse
import json
import os
import sys
import time
import wave
import zlib

import numpy as np
from pystoi import stoi

import c2codec
import opuscodec

FS = 8000
CHUNK_SAMPLES = 640		# 80 ms, four 20 ms frames
FRAMES = 4
C2_CHUNK = 32
PLC_MAX = opuscodec.PLC_MAX	# chunks of PLC before silence (decision 3)
LOSS = (0.02, 0.05, 0.10)
CLIPS_DIR = os.path.join("soaks", "clips")
D1_DIR = os.path.join("soaks", "mic_dsp_d1")
OSR = [("OSR_0010_F", "OSR_us_000_0010_8k.wav"),
       ("OSR_0030_M", "OSR_us_000_0030_8k.wav"),
       ("OSR_0011_F", "OSR_us_000_0011_8k.wav")]
# name: (bit/s, SILK forced)
OPUS = {"opus 7.2k": (7200, True), "opus 5.6k": (5600, True),
        "5.6k auto": (5600, False)}
CODECS = ["c2 3200"] + list(OPUS)
COMPLEXITY = 2


def read_wav(path):
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getsampwidth(), w.getnchannels()) != (FS, 2, 1):
            sys.exit("%s: need 8 kHz 16-bit mono" % path)
        return np.frombuffer(w.readframes(w.getnframes()), "<i2").copy()


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(np.asarray(pcm).clip(-32768, 32767).astype("<i2")
                      .tobytes())


def clips():
    """(name, condition, coder input, clean reference, copies)."""
    out = []
    for name, f in OSR:
        x = read_wav(os.path.join(CLIPS_DIR, f))
        out.append((name, "clean, as sent", x, x, 1))
    with open(os.path.join(D1_DIR, "manifest.json")) as f:
        manifest = json.load(f)
    for name, m in manifest.items():
        pcm = os.path.join(D1_DIR, name + ".pcm")
        if not os.path.exists(pcm):
            sys.exit("%s missing: python src/tdma_console/tools/mic_dsp_ab.py "
                     "make" % pcm)
        with open(pcm, "rb") as f:
            x = np.frombuffer(f.read(), "<i2").copy()
        if "%08x" % (zlib.crc32(x.tobytes()) & 0xFFFFFFFF) != m["crc32"]:
            sys.exit("%s: CRC is not the manifest's" % pcm)
        clean = read_wav(os.path.join(D1_DIR, name + "_clean.wav"))
        cond = ("%s %+.0f dB" % (m["noise"], m["snr_db"]) if m["noise"]
                else "quiet %.0f dBFS" % m["speech_dbfs"])
        out.append((name, cond, x, clean, 3))
    return out


class Coded:
    """One codec's encoding of a clip, decodable under any loss pattern."""

    def __init__(self, codec, x, complexity=COMPLEXITY):
        self.codec = codec
        self.n = len(x) // CHUNK_SAMPLES
        x = x[:self.n * CHUNK_SAMPLES]
        if codec == "c2 3200":
            self.stream = c2codec.encode(x, 3200)
        else:
            bitrate, silk = OPUS[codec]
            self.packets = opuscodec.encode(x, bitrate, complexity, silk)
            self.toc = sorted({opuscodec.toc_config(p) for p in self.packets})

    def decode(self, lost=None, plc=True):
        lost = np.zeros(self.n, bool) if lost is None else lost
        out = np.zeros(self.n * CHUNK_SAMPLES)
        if self.codec == "c2 3200":
            keep = [k for k in range(self.n) if not lost[k]]
            pcm = c2codec.decode(b"".join(
                self.stream[k * C2_CHUNK:(k + 1) * C2_CHUNK] for k in keep), 3200)
            for i, k in enumerate(keep):
                out[k * CHUNK_SAMPLES:(k + 1) * CHUNK_SAMPLES] = \
                    pcm[i * CHUNK_SAMPLES:(i + 1) * CHUNK_SAMPLES]
            return out
        chunks = [None if lost[k] else
                  b"".join(self.packets[k * FRAMES:(k + 1) * FRAMES])
                  for k in range(self.n)]
        return opuscodec.decode_chunks(chunks, FRAMES, PLC_MAX if plc else 0
                                       ).astype(np.float64)


def segment(y, clean, copies, delay):
    """The scored stretch of a decode: the middle copy of a looped clip, or
    the whole of a single one."""
    L = len(clean) if copies > 1 else len(clean) - 400
    a = (len(clean) if copies > 1 else 0) + delay
    seg = y[a:a + L]
    if len(seg) < L:
        seg = np.concatenate([seg, np.zeros(L - len(seg))])
    return clean[:L].astype(np.float64), seg


def score(y, clean, copies, delay):
    c, s = segment(y, clean, copies, delay)
    return stoi(c, s, FS), stoi(c, s, FS, extended=True)


def envelope(x, hop=32):
    f = x[:len(x) // hop * hop].reshape(-1, hop)
    return np.log10(np.mean(f ** 2, axis=1) + 1.0)


def find_delay(y, clean, copies, maxlag=400):
    ref = envelope(segment(y, clean, copies, 0)[0])
    ref -= ref.mean()
    best, lag = -2.0, 0
    for d in range(0, maxlag, 4):
        e = envelope(segment(y, clean, copies, d)[1])
        e -= e.mean()
        c = np.dot(e, ref) / (np.linalg.norm(e) * np.linalg.norm(ref) + 1e-9)
        if c > best:
            best, lag = c, d
    cands = range(max(0, lag - 12), lag + 13, 2)
    return max(cands, key=lambda d: stoi(*segment(y, clean, copies, d), FS))


def loss_pattern(name, p, seed, n):
    rng = np.random.default_rng(zlib.crc32(("%s/%g/%d" % (name, p, seed))
                                           .encode()))
    return rng.random(n) < p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--out", default=os.path.join("soaks", "opus_c0"))
    ap.add_argument("--seeds", type=int, default=10)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    print("Codec 2: %s; Opus: %s" % (c2codec.backend(), opuscodec.backend()))

    rows = {}
    for name, cond, x, clean, copies in clips():
        xin = np.tile(x, copies)
        r = {"condition": cond, "codecs": {}}
        dec0 = {}
        for codec in CODECS:
            cd = Coded(codec, xin)
            y = cd.decode()
            delay = find_delay(y, clean, copies)
            e = {"delay": delay, "lossless": score(y, clean, copies, delay)}
            if codec != "c2 3200":
                e["toc"] = cd.toc
            for p in LOSS:
                runs = [(score(cd.decode(lost), clean, copies, delay),
                         score(cd.decode(lost, plc=False), clean, copies, delay)
                         if codec != "c2 3200" else None)
                        for lost in (loss_pattern(name, p, s, cd.n)
                                     for s in range(args.seeds))]
                e["loss %g" % p] = np.mean([a for a, _ in runs], 0).tolist()
                if codec != "c2 3200":
                    e["loss %g nPLC" % p] = np.mean([b for _, b in runs],
                                                    0).tolist()
            r["codecs"][codec] = e
            dec0[codec] = (cd, delay)
        # Complexity at 7.2k, lossless only (the doc's open question).
        r["complexity 7.2k"] = {}
        for cx in (0, 2, 4):
            y = Coded("opus 7.2k", xin, cx).decode()
            d = find_delay(y, clean, copies)
            r["complexity 7.2k"][str(cx)] = score(y, clean, copies, d)
        rows[name] = r
        if copies > 1:
            gap = np.zeros(FS // 2)
            lost = loss_pattern(name, 0.05, 0, dec0["c2 3200"][0].n)
            for tag, lp in (("ab", None), ("loss_ab", lost)):
                parts = []
                for codec in ("c2 3200", "opus 7.2k"):
                    cd, d = dec0[codec]
                    parts += [segment(cd.decode(lp), clean, copies, d)[1], gap]
                write_wav(os.path.join(args.out, "%s_%s.wav" % (name, tag)),
                          np.round(np.concatenate(parts[:-1])))
        c = r["codecs"]
        print("%-10s %-15s c2 %.3f  opus7.2 %.3f (%+.3f)  5.6 %.3f  auto %.3f"
              "   delays %s   [%.0f s]" % (
                  name, cond, c["c2 3200"]["lossless"][0],
                  c["opus 7.2k"]["lossless"][0],
                  c["opus 7.2k"]["lossless"][0] - c["c2 3200"]["lossless"][0],
                  c["opus 5.6k"]["lossless"][0], c["5.6k auto"]["lossless"][0],
                  "/".join(str(c[k]["delay"]) for k in CODECS),
                  time.time() - t0))

    meta = {"codec2": c2codec.backend(), "opus": opuscodec.backend(),
            "complexity": COMPLEXITY, "plc_max_chunks": PLC_MAX,
            "seeds": args.seeds, "loss": LOSS}
    with open(os.path.join(args.out, "scores.json"), "w") as f:
        json.dump({"meta": meta, "clips": rows}, f, indent=1)
    report(rows)


def report(rows):
    clean = [n for n, r in rows.items()
             if r["condition"].startswith(("clean", "quiet"))]
    noisy = [n for n in rows if n not in clean]

    def v(n, codec, key, i):
        return rows[n]["codecs"][codec][key][i]

    for i, metric in ((0, "STOI"), (1, "ESTOI")):
        print("\n%s, lossless" % metric)
        print("%-10s %-15s %7s %9s %7s %9s %9s" % (
            "clip", "condition", "c2 3200", "opus 7.2k", "delta",
            "opus 5.6k", "5.6k auto"))
        for n, r in rows.items():
            print("%-10s %-15s %7.3f %9.3f %+7.3f %9.3f %9.3f" % (
                n, r["condition"], v(n, "c2 3200", "lossless", i),
                v(n, "opus 7.2k", "lossless", i),
                v(n, "opus 7.2k", "lossless", i) - v(n, "c2 3200", "lossless", i),
                v(n, "opus 5.6k", "lossless", i), v(n, "5.6k auto", "lossless", i)))
        for label, names in (("mean, clean", clean), ("mean, noisy", noisy)):
            m = {c: np.mean([v(n, c, "lossless", i) for n in names])
                 for c in CODECS}
            print("%-26s %7.3f %9.3f %+7.3f %9.3f %9.3f" % (
                label, m["c2 3200"], m["opus 7.2k"],
                m["opus 7.2k"] - m["c2 3200"], m["opus 5.6k"], m["5.6k auto"]))

        print("\n%s vs chunk loss (mean over clips; PLC <= %d chunks, nPLC "
              "= silence)" % (metric, PLC_MAX))
        cols = ["c2 3200", "opus 7.2k", "opus 7.2k nPLC", "opus 5.6k",
                "5.6k auto"]
        print("%-6s %-6s " % ("set", "loss") + " ".join("%14s" % c for c in cols))
        for label, names in (("clean", clean), ("noisy", noisy)):
            for p in (0,) + LOSS:
                cells = []
                for c in cols:
                    codec, npl = c.replace(" nPLC", ""), c.endswith("nPLC")
                    key = "lossless" if p == 0 else \
                        "loss %g%s" % (p, " nPLC" if npl else "")
                    cells.append("%14.3f" % np.mean([v(n, codec, key, i)
                                                     for n in names]))
                print("%-6s %4.0f %% " % (label, 100 * p) + " ".join(cells))

    print("\nOpus 7.2k complexity, lossless STOI (mean clean / noisy):")
    for cx in ("0", "2", "4"):
        print("  %s: %.3f / %.3f" % (cx, *(np.mean(
            [rows[n]["complexity 7.2k"][cx][0] for n in s]) for s in (clean, noisy))))
    print("TOC configs: " + ", ".join("%s %s" % (c, sorted({
        t for r in rows.values() for t in r["codecs"][c]["toc"]}))
        for c in OPUS) + "  (1 = SILK NB 20 ms, 17-19 = CELT NB)")
    d = np.mean([rows[n]["codecs"]["opus 7.2k"]["lossless"][0] -
                 rows[n]["codecs"]["c2 3200"]["lossless"][0] for n in clean])
    print("\nC0 STOI bar: opus 7.2k - c2 3200 on clean speech = %+.3f "
          "(pass >= +0.08): %s" % (d, "PASS" if d >= 0.08 else "FAIL"))


if __name__ == "__main__":
    main()
