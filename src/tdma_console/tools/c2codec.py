"""Codec 2 on the host, for c2clip.py and reconstruct_c2.py.

One place for the backend fallback: pycodec2 (pip; wheels for Linux, macOS
and Windows on CPython 3.13), else the c2enc / c2dec command-line tools from a
codec2 build on PATH. Both are the reference codec, so a stream decodes the
same either way.

Streams are whole: encode() takes all the PCM and decode() all the frames,
through one codec state each, which is what a persistent decoder on a
receiver would do. 8 kHz, 16-bit signed mono.
"""

import os
import shutil
import subprocess
import tempfile

import numpy as np

FS = 8000
# Codec 2 mode -> (bytes per frame, samples per frame). One frame is 20 ms at
# 1600 bit/s and up, 40 ms below.
MODES = {3200: (8, 160), 2400: (6, 160), 1600: (8, 320)}
# Mode enum in the clip header (clip_src.h). Only 3200 is sent in this task.
CODEC_ENUM = {3200: 0}

try:
    import pycodec2
except ImportError:
    pycodec2 = None


def backend():
    """Name of the backend in use; raises if there is none."""
    if pycodec2 is not None:
        return "pycodec2"
    if shutil.which("c2enc") and shutil.which("c2dec"):
        return "c2enc/c2dec"
    raise SystemExit("no Codec 2 backend: pip install pycodec2, or put a "
                     "codec2 build's c2enc and c2dec on PATH")


def frame_bytes(mode):
    return MODES[mode][0]


def frame_samples(mode):
    return MODES[mode][1]


def _cli(tool, mode, data):
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in"), os.path.join(d, "out")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run([tool, str(mode), src, dst], check=True,
                       capture_output=True)
        with open(dst, "rb") as f:
            return f.read()


def encode(pcm, mode=3200):
    """int16 PCM -> Codec 2 frames. A trailing partial frame is dropped."""
    spf = frame_samples(mode)
    pcm = np.asarray(pcm, dtype=np.int16)
    pcm = pcm[:len(pcm) // spf * spf]
    if backend() == "pycodec2":
        c = pycodec2.Codec2(mode)
        return b"".join(c.encode(pcm[i:i + spf])
                        for i in range(0, len(pcm), spf))
    return _cli("c2enc", mode, pcm.astype("<i2").tobytes())


def decode(frames, mode=3200):
    """Codec 2 frames -> int16 PCM, one decoder state for the whole stream."""
    bpf = frame_bytes(mode)
    frames = frames[:len(frames) // bpf * bpf]
    if backend() == "pycodec2":
        c = pycodec2.Codec2(mode)
        out = [c.decode(frames[i:i + bpf]) for i in range(0, len(frames), bpf)]
        return (np.concatenate(out).astype(np.int16) if out
                else np.zeros(0, np.int16))
    return np.frombuffer(_cli("c2dec", mode, frames), dtype="<i2").copy()
