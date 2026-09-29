#!/usr/bin/env python3
"""Host side of the WM8960 smoke test (apps/wm8960_smoke).

    wm_smoke.py --port COMx cmd wm stat        # any shell command
    wm_smoke.py --port COMx --sn N rec out     # 8 s of both mics -> WAVs

`rec` sends "wm rec", waits for it to finish, then reads the firmware's
wm_rec_buf over J-Link (address from the ELF) and writes out_stereo.wav,
out_L.wav and out_R.wav (8 kHz, 16-bit), with levels and a noise estimate.
"""

import argparse
import os
import subprocess
import sys
import time
import wave

import numpy as np
import pylink
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, "..", "..", "..", "build-wm8960", "wm8960_smoke",
                   "zephyr", "zephyr.elf")   # sysbuild layout
NM = "arm-zephyr-eabi-nm"
FS = 8000
REC_FRAMES = 8 * FS


def shell(port, line, wait=0.5):
    with serial.Serial(port, 115200, timeout=0.1) as s:
        s.reset_input_buffer()
        s.write((line + "\r\n").encode())
        end, out = time.time() + wait, b""
        while time.time() < end:
            out += s.read(4096)
    text = out.decode(errors="replace").replace("\r", "")
    # Drop ANSI escapes the shell sends.
    import re
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)


def symbol(name):
    out = subprocess.run([NM, ELF], capture_output=True, text=True, check=True)
    for ln in out.stdout.splitlines():
        p = ln.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    sys.exit(f"{name} not in {ELF}")


def dbfs(x):
    return 20 * np.log10(max(x, 0.5) / 32768)


def write_wav(path, data, ch):
    with wave.open(path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(data.astype("<i2").tobytes())


def rec(args):
    print(shell(args.port, "wm rec"))
    time.sleep(8.5)
    jl = pylink.JLink()
    jl.open(serial_no=args.sn)
    jl.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jl.connect("nRF5340_xxAA_APP", speed=4000)
    n = jl.memory_read32(symbol("wm_rec_frames"), 1)[0]
    raw = bytes(jl.memory_read8(symbol("wm_rec_buf"), n * 4))
    jl.close()
    x = np.frombuffer(raw, "<i2").reshape(-1, 2)
    write_wav(args.out + "_stereo.wav", x.reshape(-1), 2)
    for i, ch in enumerate("LR"):
        write_wav(f"{args.out}_{ch}.wav", x[:, i], 1)
        v = x[:, i].astype(float)
        # 20 ms frame RMS: the quietest 10 % is the noise floor.
        fr = np.sqrt((v[: len(v) // 160 * 160].reshape(-1, 160) ** 2).mean(1))
        print(f"{ch}: peak {int(abs(v).max()):6d} ({dbfs(abs(v).max()):5.1f} dBFS)"
              f"  rms {dbfs(np.sqrt((v ** 2).mean())):5.1f} dBFS"
              f"  noise floor {dbfs(np.percentile(fr, 10)):5.1f} dBFS"
              f"  loudest 20 ms {dbfs(fr.max()):5.1f} dBFS"
              f"  clipped {int((abs(v) >= 32767).sum())}")
    print(f"{n} frames -> {args.out}_stereo.wav, _L.wav, _R.wav")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--sn", type=int)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("cmd")
    c.add_argument("line", nargs="+")
    c.add_argument("--wait", type=float, default=0.5)
    r = sub.add_parser("rec")
    r.add_argument("out")
    args = ap.parse_args()
    if args.cmd == "cmd":
        print(shell(args.port, " ".join(args.line), args.wait))
    else:
        if args.sn is None:
            sys.exit("rec needs --sn (J-Link serial)")
        rec(args)


if __name__ == "__main__":
    main()
