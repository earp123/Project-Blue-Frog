#!/usr/bin/env python3
"""Bench port to a unit over J-Link RTT (CONFIG_SOAK_RTT).

Replaces the SD card on the bench: a soak's records stream to the host as
they are made, and a small command channel starts and stops soaks. The
firmware side is src/tdma_console/rtt_link.{c,h}; the task write-up is
docs/rtt_link_c2_transport.md.

    up 1    soak records, the same 64 B stream the card gets (META first)
    up 2    one text reply per command: "ok ..." or "err <reason>"
    down 1  text commands, "\\n"-terminated, plus clip bytes

Usage:
    rtt_link.py --sn N status
    rtt_link.py --sn N soak 5 [--sd]          # 0 = continuous
    rtt_link.py --sn N stop
    rtt_link.py --sn N mode ramp|tone|clip
    rtt_link.py --sn N lead 20000             # stage 20 ms before TX; 0 = at once
    rtt_link.py --sn N clip clip_F.c2         # "clip <n> <crc>" + the bytes
    rtt_link.py --sn N capture -o m.bin [--minutes M] [--soak M [--sd]]
    rtt_link.py --all capture -o soaks/rtt/ [--soak M]
    rtt_link.py --list                        # connected J-Link serials

Start capture before the soak for a gap-free file: the unit's 8 KB up buffer
covers a late start by ~2.5 s, no more. With --soak the capture sends the
soak command itself once it is draining, and ends when the run does. Without
it, capture ends on --minutes, Ctrl-C, or once records stop for --idle-exit
seconds after having flowed (the run ended). A capture file is a soak log:
decode_soak_log.py and reconstruct_tone.py read it unchanged.

Attaching never resets or halts the target, and `west flash` still works
between sessions. The J-Link DLL allows one connection per process, so
--all runs one child process per unit. Needs pylink-square (pip).
"""

import argparse
import datetime
import os
import struct
import subprocess
import sys
import time
import zlib

DEVICE = "nRF5340_xxAA_APP"
UP_SOAK, UP_CTL, DOWN_CTL = 1, 2, 1
REC_SIZE = 64
REC_META = 1
POLL_S = 0.005          # up-buffer drain period
REPLY_TIMEOUT_S = 10.0  # stop waits up to 5 s on the card writer


def _pylink():
    try:
        import pylink
    except ImportError:
        sys.exit("rtt_link.py needs pylink-square: pip install pylink-square")
    return pylink


def list_serials():
    pylink = _pylink()
    jl = pylink.JLink()
    return [int(e.SerialNumber) for e in jl.connected_emulators()]


class Link:
    """One attached unit. Connecting does not reset or halt it."""

    def __init__(self, sn):
        pylink = _pylink()
        self.jl = pylink.JLink()
        self.jl.open(serial_no=sn)
        self.jl.set_tif(pylink.enums.JLinkInterfaces.SWD)
        self.jl.connect(DEVICE, speed=4000)
        self.jl.rtt_start()
        # The DLL finds the control block by scanning RAM; the buffer
        # queries raise until it has.
        deadline = time.monotonic() + 5.0
        while True:
            try:
                n_up = self.jl.rtt_get_num_up_buffers()
                n_down = self.jl.rtt_get_num_down_buffers()
                break
            except pylink.errors.JLinkRTTException:
                if time.monotonic() > deadline:
                    raise SystemExit(
                        f"J-Link {sn}: no RTT control block found. Is the "
                        "unit running a CONFIG_SOAK_RTT build?")
                time.sleep(0.05)
        if n_up <= UP_CTL or n_down <= DOWN_CTL:
            raise SystemExit(
                f"J-Link {sn}: RTT has {n_up} up / {n_down} down buffers; "
                "the bench port needs 3 / 2 (CONFIG_SOAK_RTT build?)")
        self.sn = sn
        self._reply_buf = b""

    def close(self):
        try:
            self.jl.rtt_stop()
        finally:
            self.jl.close()

    def read(self, ch, n=4096):
        return bytes(self.jl.rtt_read(ch, n))

    def write(self, data, timeout=5.0):
        """Push all of data into down 1, waiting for the unit to drain it."""
        deadline = time.monotonic() + timeout
        while data:
            n = self.jl.rtt_write(DOWN_CTL, list(data))
            data = data[n:]
            if data:
                if time.monotonic() > deadline:
                    raise SystemExit(f"J-Link {self.sn}: unit stopped "
                                     "reading commands")
                time.sleep(0.002)

    def flush_replies(self):
        while self.read(UP_CTL):
            pass
        self._reply_buf = b""

    def reply(self, timeout=REPLY_TIMEOUT_S):
        deadline = time.monotonic() + timeout
        while b"\n" not in self._reply_buf:
            if time.monotonic() > deadline:
                raise SystemExit(f"J-Link {self.sn}: no reply within "
                                 f"{timeout:.0f} s")
            chunk = self.read(UP_CTL, 512)
            if chunk:
                self._reply_buf += chunk
            else:
                time.sleep(POLL_S)
        line, self._reply_buf = self._reply_buf.split(b"\n", 1)
        return line.decode("ascii", "replace").strip()

    def command(self, line, payload=b""):
        self.flush_replies()
        self.write(line.encode("ascii") + b"\n" + payload)
        return self.reply()


def parse_status(line):
    """'ok status role=M slot=0 ...' -> {'role': 'M', 'slot': '0', ...}"""
    return dict(tok.split("=", 1) for tok in line.split()[2:] if "=" in tok)


class RecCounter:
    """Live record count, rate and seq gaps from the raw stream."""

    def __init__(self):
        self.pending = b""
        self.records = 0
        self.runs = 0
        self.gaps = 0
        self.missing = 0
        self.next_seq = None

    def feed(self, data):
        self.pending += data
        whole = len(self.pending) // REC_SIZE * REC_SIZE
        for off in range(0, whole, REC_SIZE):
            rtype = self.pending[off]
            (seq,) = struct.unpack_from("<I", self.pending, off + 16)
            if rtype == REC_META and seq == 0:
                self.runs += 1          # seq restarts with every run
            elif self.next_seq is not None and seq != self.next_seq:
                self.gaps += 1
                self.missing += (seq - self.next_seq) & 0xFFFFFFFF
            self.next_seq = (seq + 1) & 0xFFFFFFFF
            self.records += 1
        self.pending = self.pending[whole:]


def capture_path(link, out):
    """-o is a file, or a directory to name the file in from the unit."""
    if not (out.endswith(("/", "\\")) or os.path.isdir(out)):
        return out
    os.makedirs(out, exist_ok=True)
    st = parse_status(link.command("status"))
    unit = ("M" if st.get("role") == "M" else "S") + st.get("slot", "x")
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return os.path.join(out, f"{unit}_{link.sn}_{stamp}.bin")


def cmd_capture(link, args):
    path = capture_path(link, args.out)
    # Stale records from before this capture would open the file mid-run.
    while link.read(UP_SOAK):
        pass

    cnt = RecCounter()
    t0 = last_data = last_print = time.monotonic()
    seen_data = False
    soak_sent = args.soak is None
    why = "Ctrl-C"
    print(f"[{link.sn}] capturing to {path}", flush=True)
    try:
        with open(path, "wb") as f:
            while True:
                data = link.read(UP_SOAK)
                now = time.monotonic()
                if data:
                    f.write(data)
                    cnt.feed(data)
                    seen_data = True
                    last_data = now
                else:
                    time.sleep(POLL_S)

                if not soak_sent:
                    # Draining already, so the run's first records land.
                    line = f"soak {args.soak}" + (" sd" if args.sd else "")
                    r = link.command(line)
                    print(f"[{link.sn}] {line}: {r}", flush=True)
                    if not r.startswith("ok"):
                        why = "soak refused"
                        break
                    soak_sent = True
                    last_data = now

                if now - last_print >= 5.0:
                    last_print = now
                    el = now - t0
                    print(f"[{link.sn}] {el:6.0f} s  {cnt.records} rec  "
                          f"{cnt.records / el:5.1f} rec/s  gaps {cnt.gaps} "
                          f"(missing {cnt.missing})", flush=True)

                if args.minutes and now - t0 >= args.minutes * 60:
                    why = "time limit"
                    break
                if (args.idle_exit and seen_data and
                        now - last_data >= args.idle_exit):
                    why = "run ended"
                    break
    except KeyboardInterrupt:
        pass

    el = max(time.monotonic() - t0, 1e-9)
    print(f"[{link.sn}] done ({why}): {cnt.records} records in {el:.0f} s, "
          f"{cnt.runs} run(s), seq gaps {cnt.gaps} (missing {cnt.missing}), "
          f"{len(cnt.pending)} trailing bytes -> {path}", flush=True)
    return 0 if cnt.gaps == 0 else 1


def cmd_clip(link, args):
    with open(args.file, "rb") as f:
        data = f.read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"clip {args.file}: {len(data)} B, crc32 {crc:08x}")
    r = link.command(f"clip {len(data)} {crc:08x}", payload=data)
    print(r)
    return 0 if r.startswith("ok") else 1


def run_one(args):
    link = Link(args.sn)
    try:
        if args.cmd == "status":
            line = "status"
        elif args.cmd == "soak":
            line = f"soak {args.minutes}" + (" sd" if args.sd else "")
        elif args.cmd == "stop":
            line = "stop"
        elif args.cmd == "mode":
            line = f"mode {args.mode}"
        elif args.cmd == "lead":
            line = f"lead {args.us}"
        elif args.cmd == "clip":
            return cmd_clip(link, args)
        elif args.cmd == "capture":
            return cmd_capture(link, args)
        r = link.command(line)
        print(r)
        return 0 if r.startswith("ok") else 1
    finally:
        link.close()


def run_all(args, argv):
    """One child process per connected unit (one J-Link per process)."""
    if args.cmd == "capture" and not (args.out.endswith(("/", "\\")) or
                                      os.path.isdir(args.out)):
        sys.exit("--all capture needs -o to be a directory")
    serials = list_serials()
    if not serials:
        sys.exit("no J-Link emulators connected")
    rest = [a for a in argv if a != "--all"]
    procs = [subprocess.Popen([sys.executable, os.path.abspath(__file__),
                               "--sn", str(sn)] + rest)
             for sn in serials]
    rc = 0
    try:
        for p in procs:
            rc |= p.wait()
    except KeyboardInterrupt:
        # Ctrl-C reaches the children too; let them close their files.
        for p in procs:
            rc |= p.wait()
    return rc


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    who = ap.add_mutually_exclusive_group(required=True)
    who.add_argument("--sn", type=int, help="J-Link serial number")
    who.add_argument("--all", action="store_true",
                     help="every connected J-Link, one process each")
    who.add_argument("--list", action="store_true",
                     help="print connected J-Link serials and exit")
    sub = ap.add_subparsers(dest="cmd")

    sub.add_parser("status")
    p = sub.add_parser("soak")
    p.add_argument("minutes", type=int, help="0 = continuous")
    p.add_argument("--sd", action="store_true", help="also log to the card")
    sub.add_parser("stop")
    p = sub.add_parser("mode")
    p.add_argument("mode", choices=["ramp", "tone", "clip"])
    p = sub.add_parser("lead")
    p.add_argument("us", type=int,
                   help="stage each payload this many us before the unit's "
                        "TX boundary, from the next soak (0 = at once)")
    p = sub.add_parser("clip")
    p.add_argument("file", help="Codec 2 clip, a multiple of 32 B")
    p = sub.add_parser("capture")
    p.add_argument("-o", "--out", required=True,
                   help="output file, or a directory to name it in")
    p.add_argument("--minutes", type=float, default=0,
                   help="stop capturing after this long (0 = no limit)")
    p.add_argument("--soak", type=int, metavar="M",
                   help="also start an M-minute soak (0 = continuous) once "
                        "capturing")
    p.add_argument("--sd", action="store_true",
                   help="with --soak: also log to the card")
    p.add_argument("--idle-exit", type=float, default=3.0, metavar="S",
                   help="end once records stop for S seconds after flowing "
                        "(0 = never; default 3)")

    args = ap.parse_args(argv)
    if args.list:
        for sn in list_serials():
            print(sn)
        return 0
    if args.cmd is None:
        ap.error("a command is required")
    if args.all:
        return run_all(args, argv)
    return run_one(args)


if __name__ == "__main__":
    sys.exit(main())
