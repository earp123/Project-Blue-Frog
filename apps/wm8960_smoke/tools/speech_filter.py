#!/usr/bin/env python3
"""Band-limit 8 kHz speech clips for small speakers.

    speech_filter.py in.pcm out.pcm [--hp 300] [--lp 3400] [--peak -1]

Raw 16-bit little-endian mono PCM at 8 kHz in and out; a WAV of the result
is written next to it for listening on the PC. The chain:

  - 4th-order Butterworth high-pass at --hp (default 300 Hz): the room
    rumble and chest resonance a small speaker cannot reproduce, and which
    would only eat its excursion and the amplifier's headroom.
  - 4th-order Butterworth low-pass at --lp (default 3400 Hz): the top of
    the telephone band, clear of the 4 kHz Nyquist edge.
  - Peak-normalised to --peak dBFS (default -1).

Biquads from the RBJ Audio EQ Cookbook, run forward and then backward
(zero phase, so the filtering does not smear consonants). NumPy only.
"""

import argparse
import math
import wave

import numpy as np

FS = 8000
# Butterworth 4th order = two biquads with these Qs.
Q4 = (1 / (2 * math.cos(math.pi / 8)), 1 / (2 * math.cos(3 * math.pi / 8)))


def biquad(kind, f0, q):
    w = 2 * math.pi * f0 / FS
    c, a = math.cos(w), math.sin(w) / (2 * q)
    if kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    else:
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
    den = [1 + a, -2 * c, 1 - a]
    return [x / den[0] for x in b], [x / den[0] for x in den]


def run(b, a, x):
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def zero_phase(stages, x):
    for b, a in stages:
        x = run(b, a, x)
    x = x[::-1]
    for b, a in stages:
        x = run(b, a, x)
    return x[::-1]


def band_energy(x, lo, hi):
    p = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    return p[(f >= lo) & (f < hi)].sum() / p.sum()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--hp", type=float, default=300.0)
    ap.add_argument("--lp", type=float, default=3400.0)
    ap.add_argument("--peak", type=float, default=-1.0)
    args = ap.parse_args()

    x = np.fromfile(args.inp, "<i2").astype(float)
    # Two biquads per corner (4th order); forward + backward squares the
    # magnitude: 48 dB/octave outside the band, -6 dB at each corner.
    stages = [biquad("hp", args.hp, Q4[0]), biquad("hp", args.hp, Q4[1]),
              biquad("lp", args.lp, Q4[0]), biquad("lp", args.lp, Q4[1])]
    y = zero_phase(stages, x)
    y *= 32767 * 10 ** (args.peak / 20) / np.abs(y).max()
    y = np.clip(np.round(y), -32768, 32767).astype("<i2")
    y.tofile(args.out)
    wav = args.out.rsplit(".", 1)[0] + ".wav"
    with wave.open(wav, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(y.tobytes())

    def db(v):
        return 20 * math.log10(max(v, 1e-9) / 32768)

    for name, s in (("in ", x), ("out", y.astype(float))):
        print(f"{name}: peak {db(np.abs(s).max()):6.1f} dBFS  rms "
              f"{db(np.sqrt((s ** 2).mean())):6.1f} dBFS  energy <300 Hz "
              f"{100 * band_energy(s, 0, 300):4.1f} %  300-3400 Hz "
              f"{100 * band_energy(s, 300, 3400):4.1f} %  >3400 Hz "
              f"{100 * band_energy(s, 3400, 4001):4.1f} %")
    print(f"-> {args.out}, {wav}")


if __name__ == "__main__":
    main()
