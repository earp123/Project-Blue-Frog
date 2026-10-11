#!/usr/bin/env python3
"""Mic DSP front end, gate D1: a repeatable A/B in pcm mode, scored with STOI.

docs/mic_dsp_frontend.md. Instead of a talker and speakers, the unit encodes
a prepared clip (`mode pcm`): clean speech, or speech mixed with crowd babble
or wind rumble at a set SNR. Every run feeds the encoder the same samples, so
DSP off and DSP on differ only by the DSP, and the clean speech under the mix
is a sample-aligned STOI reference.

    mic_dsp_ab.py make [-o soaks/mic_dsp_d1]          # build the clips
    mic_dsp_ab.py run --sn N [-o DIR] [clip ...]      # DSP off, then on
    mic_dsp_ab.py run --sn N --stages n --label ns clip ...   # one variant
    mic_dsp_ab.py score [DIR]                         # table + WAVs

make writes, per condition, <name>.pcm (8 s, the clip buffer's size),
<name>_clean.wav (the speech alone: the reference) and <name>_in.wav (what
the encoder is fed). Speech: the Open Speech Repository clips that
c2clip.py fetches into soaks/clips. Crowd: ten overlapping streams cut from
the same recordings (other sentences, other talkers) plus pink noise, a
dense babble. Roar: forty streams and pink noise at the same level, close
to stationary: the best case for a noise suppressor. Wind: low-passed brown noise with slow gusts, mostly below
200 Hz. The target talker's active speech is set to -22 dBFS RMS, where the
bench mics sat, except "quiet", at -34 dBFS, for the AGC.

run needs the unit's role picked (any slot; it runs alone). Per clip it
uploads the PCM, sets mode pcm, phase 12000 and rawmic on, then captures a
1-minute soak with "dsp off" and one with "dsp on" (all stages), saving
<name>_off.bin/.raw, <name>_on.bin/.raw and the unit's status after each.

score decodes each run's own TX records with the reference Codec 2, places
chunk n at clip position n mod 100, and scores loops 2 onwards (16 s in:
the noise estimate and the AGC have settled) against the clean speech with
STOI and ESTOI (pystoi). Delay is found per run by envelope correlation
against the encoder's input, so the DSP's 10 ms and Codec 2's own delay do
not bias anything. Rows:
    in        the encoder's input, uncoded
    cond      after the DSP, uncoded (the "RC" tap of the DSP-on run)
    c2 off    Codec 2 of the input (DSP off): what is sent today
    c2 on     Codec 2 of the conditioned input (DSP on)
D1 passes in noise if c2 on beats c2 off by >= 0.05 STOI (and by ear), and
in quiet if they are within 0.02 with no clipping. score also writes
<name>_ab.wav (one steady loop: left c2 off, right c2 on) to listen to.
"""

import argparse
import glob
import json
import os
import sys
import wave
import zlib

import numpy as np

import c2codec
from decode_soak_log import REC_TX, iter_records
from raw_mic_wav import parse_raw, timeline
from reconstruct_c2 import HDR, MODE, SAMPLES_PER_CHUNK, Unwrap, header
from reconstruct_tone import trusted_records, write_wav

FS = c2codec.FS
CHUNKS = 100                       # 8 s: the clip buffer holds 128 KB
CLIP = CHUNKS * SAMPLES_PER_CHUNK
SPEECH_DBFS = -22.0
CLIPS_DIR = os.path.join("soaks", "clips")
OSR = {"M": "OSR_us_000_0030_8k.wav", "F": "OSR_us_000_0010_8k.wav",
       "F2": "OSR_us_000_0011_8k.wav"}

# name: (talker, noise, SNR dB, speech level dBFS)
CONDITIONS = {
    "quiet_M":  ("M", None, None, SPEECH_DBFS),
    "quiet_F":  ("F", None, None, SPEECH_DBFS),
    "soft_M":   ("M", None, None, -34.0),
    "crowd5_M": ("M", "crowd", 5.0, SPEECH_DBFS),
    "crowd0_M": ("M", "crowd", 0.0, SPEECH_DBFS),
    "crowd0_F": ("F", "crowd", 0.0, SPEECH_DBFS),
    "wind0_M":  ("M", "wind", 0.0, SPEECH_DBFS),
    "roar0_M":  ("M", "roar", 0.0, SPEECH_DBFS),
}


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getframerate() != FS or w.getsampwidth() != 2:
            sys.exit("%s: need 8 kHz 16-bit" % path)
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2")
        if w.getnchannels() == 2:
            x = x[::2]
    return x.astype(np.float64)


def active_rms(x):
    """RMS over 20 ms frames within 30 dB of the loudest: the speech."""
    f = x[:len(x) // 160 * 160].reshape(-1, 160)
    e = np.mean(f ** 2, axis=1) + 1e-9
    act = e > e.max() * 1e-3
    return np.sqrt(np.mean(e[act]))


def dbfs(rms):
    return 20 * np.log10(max(rms, 1e-3) / 32768)


def pink(n, rng):
    """Voss-McCartney-ish pink noise: summed octave-held white sources."""
    out = np.zeros(n)
    for k in range(12):
        step = 2 ** k
        v = rng.standard_normal(n // step + 2)
        out += np.repeat(v, step)[:n]
    return out


def crowd(n, rng, talker, streams=10, pink_db=-8.0):
    """Babble: streams cut from all three recordings, the target's own
    sentences excluded, plus pink noise pink_db relative to the babble.
    The default is a crowd near the talker; "roar" (40 streams, pink at
    0 dB) is the near-stationary roar of a stadium heard from inside it."""
    src = {k: read_wav(os.path.join(CLIPS_DIR, f)) for k, f in OSR.items()}
    out = np.zeros(n)
    for _ in range(streams):
        k = rng.choice(list(src))
        x = src[k]
        lo = CLIP if k == talker else 0       # skip the target's 8 s
        start = rng.integers(lo, len(x) - n)
        seg = x[start:start + n]
        out += seg / (active_rms(seg) + 1e-9) * 10 ** (rng.uniform(-3, 3) / 20)
    out /= np.sqrt(np.mean(out ** 2))
    p = pink(n, rng)
    out += p / np.sqrt(np.mean(p ** 2)) * 10 ** (pink_db / 20)
    return out


def wind(n, rng):
    """Brown noise through a ~80 Hz one-pole low-pass, gusting at 0.2-1 Hz."""
    w = np.cumsum(rng.standard_normal(n))
    w -= np.convolve(w, np.ones(4001) / 4001, "same")     # drift out
    a = np.exp(-2 * np.pi * 80 / FS)
    y = np.zeros(n)
    acc = 0.0
    for i in range(n):
        acc = a * acc + (1 - a) * w[i]
        y[i] = acc
    t = np.arange(n) / FS
    gust = 1 + 0.6 * np.sin(2 * np.pi * 0.3 * t) + 0.3 * np.sin(2 * np.pi * 0.9 * t + 1)
    y *= gust
    return y / np.sqrt(np.mean(y ** 2))


def cmd_make(args):
    os.makedirs(args.out, exist_ok=True)
    manifest = {}
    for name, (talker, noise, snr, level) in CONDITIONS.items():
        rng = np.random.default_rng(zlib.crc32(name.encode()))
        speech = read_wav(os.path.join(CLIPS_DIR, OSR[talker]))[:CLIP]
        speech *= 10 ** (level / 20) * 32768 / active_rms(speech)
        mix = speech.copy()
        if noise:
            nz = (crowd(CLIP, rng, talker) if noise == "crowd" else
                  crowd(CLIP, rng, talker, 40, 0.0) if noise == "roar" else
                  wind(CLIP, rng))
            nz *= active_rms(speech) / 10 ** (snr / 20)
            mix += nz
        peak = np.abs(mix).max()
        if peak > 32000:
            sys.exit("%s clips (peak %.0f)" % (name, peak))
        pcm = np.round(mix).astype("<i2")
        with open(os.path.join(args.out, name + ".pcm"), "wb") as f:
            f.write(pcm.tobytes())
        write_wav(os.path.join(args.out, name + "_clean.wav"),
                  np.round(speech).astype(np.int16), FS)
        write_wav(os.path.join(args.out, name + "_in.wav"), pcm, FS)
        manifest[name] = {"talker": talker, "noise": noise, "snr_db": snr,
                          "speech_dbfs": level,
                          "crc32": "%08x" % (zlib.crc32(pcm.tobytes()) & 0xFFFFFFFF)}
        print("%-9s %s, %s, speech %.0f dBFS, peak %.0f -> %s.pcm"
              % (name, talker, "%s at %+.0f dB SNR" % (noise, snr) if noise
                 else "no noise", level, peak, name))
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)


def cmd_run(args):
    import rtt_link
    with open(os.path.join(args.out, "manifest.json")) as f:
        manifest = json.load(f)
    names = args.clips or list(manifest)
    link = rtt_link.Link(args.sn)
    try:
        st = rtt_link.parse_status(link.command("status").splitlines()[0])
        if st.get("role", "-") == "-":
            sys.exit("[%s] pick the unit's role first" % args.sn)
        for name in names:
            with open(os.path.join(args.out, name + ".pcm"), "rb") as f:
                data = f.read()
            crc = zlib.crc32(data) & 0xFFFFFFFF
            stages = [("dsp %s %s" % (st, "on" if st[0] in args.stages
                                          else "off"), b"")
                      for st in ("hpf", "ns", "agc")]
            for line, payload in [("clip %d %08x" % (len(data), crc), data),
                                  ("mode pcm", b""), ("phase 12000", b""),
                                  ("rawmic on", b"")] + stages:
                r = link.command(line, payload=payload)
                if not r.startswith("ok"):
                    sys.exit("[%s] %s: %s" % (args.sn, line, r))
            runs = (("off", "off"), ("on", "on")) if args.label == "on" else                 (("on", args.label),)
            for dsp, label in runs:
                link.command("dsp " + dsp)
                path = os.path.join(args.out, "%s_%s.bin" % (name, label))
                cap = argparse.Namespace(out=path, soak=1, sd=False,
                                         minutes=0, idle_exit=3.0)
                rtt_link.cmd_capture(link, cap)
                with open(path[:-4] + "_status.txt", "w") as f:
                    f.write(link.command("status") + "\n")
        link.command("rawmic off")
        link.command("dsp on")
        for st in ("hpf", "ns", "agc"):
            link.command("dsp %s on" % st)
    finally:
        link.close()


def tx_timeline(path):
    """Decoded own-TX audio, chunk u at u * 640; and the chunks present."""
    with open(path, "rb") as f:
        recs = list(iter_records(f.read()))
    good, _ = trusted_records(recs)
    unwrap = Unwrap()
    chunks = {}
    for r in good:
        if r.type != REC_TX:
            continue
        h = header(r.payload)
        if h is None:
            continue
        chunks.setdefault(unwrap(h[0]), bytes(r.payload[HDR:HDR + 32]))
    if not chunks:
        return None, set()
    hi = max(chunks)
    # One decoder state through the run, as a receiver would have; a gap
    # (none expected on a unit's own TX) decodes as silence frames.
    stream = b"".join(chunks.get(u, bytes(32)) for u in range(hi + 1))
    pcm = c2codec.decode(stream, MODE).astype(np.float64)
    return pcm, set(chunks)


def tap_timeline(path, tap):
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        frames, _ = parse_raw(f.read(), tap)
    if not frames:
        return None
    hi = max(n for n, _ in frames)
    return timeline(frames, 0, hi + 1).astype(np.float64)


def envelope(x, hop=32):
    f = x[:len(x) // hop * hop].reshape(-1, hop)
    return np.log10(np.mean(f ** 2, axis=1) + 1.0)


def find_delay(out, ref, start, maxlag=960):
    """Delay of out vs ref (samples), by 4 ms log-envelope correlation over
    one loop starting at sample start; refined to 1 sample on waveforms."""
    seg = ref[start:start + CLIP]
    e_ref = envelope(seg)
    e_ref -= e_ref.mean()
    best, lag = -2.0, 0
    for d in range(0, maxlag, 32):
        e = envelope(out[start + d:start + d + CLIP])
        if len(e) != len(e_ref):
            break
        e = e - e.mean()
        c = np.dot(e, e_ref) / (np.linalg.norm(e) * np.linalg.norm(e_ref) + 1e-9)
        if c > best:
            best, lag = c, d
    return lag


def loops_present(chunks, total):
    """Steady loops L >= 2 whose chunks and the next loop's are all there."""
    out = []
    for L in range(2, total // CHUNKS):
        need = range(L * CHUNKS, min((L + 2) * CHUNKS, total))
        if all(u in chunks for u in need) and (L + 1) * CHUNKS < total:
            out.append(L)
    return out


def score_signal(out, delay, loops, clean):
    from pystoi import stoi
    s, e = [], []
    for L in loops:
        a = L * CLIP + delay
        seg = out[a:a + CLIP]
        if len(seg) < CLIP:
            continue
        s.append(stoi(clean, seg, FS, extended=False))
        e.append(stoi(clean, seg, FS, extended=True))
    return (float(np.mean(s)), float(np.mean(e))) if s else (None, None)


def cmd_score(args):
    from pystoi import stoi
    d = args.dir
    with open(os.path.join(d, "manifest.json")) as f:
        manifest = json.load(f)
    rows = {}
    for name, m in manifest.items():
        if not os.path.exists(os.path.join(d, name + "_on.bin")):
            continue
        clean = read_wav(os.path.join(d, name + "_clean.wav"))
        x = np.frombuffer(open(os.path.join(d, name + ".pcm"), "rb").read(),
                          "<i2").astype(np.float64)
        r = {"in": (stoi(clean, x, FS), stoi(clean, x, FS, extended=True))}
        ab = {}
        for dsp in ("off", "on"):
            pcm, chunks = tx_timeline(os.path.join(d, "%s_%s.bin" % (name, dsp)))
            if pcm is None:
                continue
            total = len(pcm) // SAMPLES_PER_CHUNK
            loops = loops_present(chunks, total)
            inp = np.tile(x, total // CHUNKS + 2)[:len(pcm)]
            delay = find_delay(pcm, inp, loops[0] * CLIP) if loops else 0
            r["c2 " + dsp] = score_signal(pcm, delay, loops, clean)
            r["delay " + dsp] = delay
            r["loops " + dsp] = len(loops)
            if loops:
                a = loops[-1] * CLIP + delay
                ab[dsp] = pcm[a:a + CLIP]
            if dsp == "on":
                cond = tap_timeline(os.path.join(d, name + "_on.raw"), b"RC")
                if cond is not None:
                    n = len(cond) // SAMPLES_PER_CHUNK
                    cl = [L for L in range(2, n // CHUNKS - 1)]
                    cinp = np.tile(x, n // CHUNKS + 2)[:len(cond)]
                    cd = find_delay(cond, cinp, cl[0] * CLIP) if cl else 0
                    r["cond"] = score_signal(cond, cd, cl, clean)
                    r["delay cond"] = cd
                    if cl:
                        a = cl[-1] * CLIP + cd
                        seg = cond[a:a + CLIP]
                        r["cond dbfs"] = dbfs(active_rms(seg))
                        write_wav(os.path.join(d, name + "_cond.wav"),
                                  np.round(seg).astype(np.int16), FS)
        if "off" in ab and "on" in ab:
            with wave.open(os.path.join(d, name + "_ab.wav"), "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(FS)
                w.writeframes(np.stack([ab["off"], ab["on"]], 1)
                              .clip(-32768, 32767).astype("<i2").tobytes())
        for path in sorted(glob.glob(os.path.join(d, name + "_*.bin"))):
            label = os.path.basename(path)[len(name) + 1:-4]
            if label in ("off", "on"):
                continue
            pcm, chunks = tx_timeline(path)
            if pcm is None:
                continue
            total = len(pcm) // SAMPLES_PER_CHUNK
            loops = loops_present(chunks, total)
            inp = np.tile(x, total // CHUNKS + 2)[:len(pcm)]
            delay = find_delay(pcm, inp, loops[0] * CLIP) if loops else 0
            r["c2 " + label] = score_signal(pcm, delay, loops, clean)
        rows[name] = r

    def f(v, i):
        return "  -  " if v is None or v[i] is None else "%.3f" % v[i]

    print("%-9s %-12s  STOI: %5s %5s %6s %6s %7s | ESTOI: %5s %5s %6s %6s %7s"
          % ("clip", "condition", "in", "cond", "c2off", "c2on", "delta",
             "in", "cond", "c2off", "c2on", "delta"))
    for name, r in rows.items():
        m = manifest[name]
        cond = ("%s %+.0f dB" % (m["noise"], m["snr_db"]) if m["noise"]
                else "quiet %.0f dBFS" % m["speech_dbfs"])
        dl = []
        for i in (0, 1):
            off, on = r.get("c2 off"), r.get("c2 on")
            dl.append("%+.3f" % (on[i] - off[i]) if off and on and off[i]
                      is not None and on[i] is not None else "  -  ")
        print("%-9s %-12s  STOI: %5s %5s %6s %6s %7s | ESTOI: %5s %5s %6s %6s %7s"
              % (name, cond, f(r["in"], 0), f(r.get("cond"), 0),
                 f(r.get("c2 off"), 0), f(r.get("c2 on"), 0), dl[0],
                 f(r["in"], 1), f(r.get("cond"), 1), f(r.get("c2 off"), 1),
                 f(r.get("c2 on"), 1), dl[1]))
    print("\ndelays (samples): " + ", ".join(
        "%s off %s / on %s / cond %s" % (n, r.get("delay off"), r.get("delay on"),
                                         r.get("delay cond"))
        for n, r in rows.items()))
    extra = sorted({k[3:] for r in rows.values() for k in r
                    if k.startswith("c2 ") and k[3:] not in ("off", "on")})
    for label in extra:
        print("variant %-6s STOI (delta vs c2 off): " % label + ", ".join(
            "%s %.3f (%+.3f)" % (n, r["c2 " + label][0],
                                 r["c2 " + label][0] - r["c2 off"][0])
            for n, r in rows.items() if "c2 " + label in r and r.get("c2 off")))
    print("loops scored: " + ", ".join(
        "%s %s/%s" % (n, r.get("loops off"), r.get("loops on"))
        for n, r in rows.items()))
    print("conditioned speech level (dBFS): " + ", ".join(
        "%s %.1f" % (n, r["cond dbfs"]) for n, r in rows.items()
        if "cond dbfs" in r))
    with open(os.path.join(d, "scores.json"), "w") as fh:
        json.dump(rows, fh, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make")
    p.add_argument("-o", "--out", default=os.path.join("soaks", "mic_dsp_d1"))
    p = sub.add_parser("run")
    p.add_argument("--sn", type=int, required=True)
    p.add_argument("-o", "--out", default=os.path.join("soaks", "mic_dsp_d1"))
    p.add_argument("--stages", default="hna",
                   help="stages for the DSP run: h(pf) n(s) a(gc)")
    p.add_argument("--label", default="on",
                   help="'on' runs off then on; any other label runs only "
                        "the DSP with --stages, saved as <clip>_<label>")
    p.add_argument("clips", nargs="*")
    p = sub.add_parser("score")
    p.add_argument("dir", nargs="?", default=os.path.join("soaks", "mic_dsp_d1"))
    args = ap.parse_args()
    {"make": cmd_make, "run": cmd_run, "score": cmd_score}[args.cmd](args)


if __name__ == "__main__":
    main()
