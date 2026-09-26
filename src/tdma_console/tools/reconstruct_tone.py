#!/usr/bin/env python3
"""Rebuild the peers' audio from a tone soak log (CONFIG_SOAK_PAYLOAD_TONE).

Every RX record carries the peer's 40-byte payload, which in a tone build is
40 samples of that peer's tone (8 kHz, 8-bit, f0 * (slot + 1)). This lays each
peer's chunks on a common frame timeline, mixes the peers and writes a WAV, so
a lost frame is an audible gap.

Usage:
    python reconstruct_tone.py P0_5M_000.BIN              # -> P0_5M_000.wav
    python reconstruct_tone.py P0_5M_000.BIN -o mix.wav --per-slot

The WAV is NOT real time: a frame carries 40 B every 80 ms (4 kbps), which is
5 ms of 8 kHz audio, so playback is the run compressed 16x.

Rules (docs/tone_payload_test.md): a bad packet costs its own frame, nothing
more. Chunks are placed by arrival time (t_us), never by frame_ctr, which
secondaries only echo from the master's beacon. Duplicates are byte-identical
repeats; gaps are silence; nothing is concealed.
"""

import argparse
import math
import os
import sys
import wave
from collections import namedtuple

import numpy as np

from decode_soak_log import (PAYLOAD_TONE, REC_META, REC_RX, iter_records,
                             parse_meta, payload_desc)

# Mirrors src/tdma_console/tone_src.{c,h}; any change there must land here.
TONE_SILENCE = 128
TONE_AMPL = 100
TONE_LUT = np.array([TONE_SILENCE + math.floor(
    TONE_AMPL * math.sin(2 * math.pi * i / 256) + 0.5) for i in range(256)],
    dtype=np.uint8)

# A chunk matches an expected one if no sample is further off than this. With
# packet CRC on, matches are exact except where the tone's near-periodicity
# moves a sample across one LUT step (the steepest step is 3).
MATCH_TOL = 3

U32 = 1 << 32


def tone_delta(f_hz, fs_hz):
    """Phase step per sample, 2^32 = one cycle: round(f * 2^32 / fs)."""
    return ((f_hz << 32) + fs_hz // 2) // fs_hz


def tone_chunk(delta, n, chunk):
    """Chunk n of a tone, exactly as tone_src_fill() computes it."""
    phase0 = (n * chunk * delta) % U32
    phases = (phase0 + np.arange(chunk, dtype=np.uint64) * delta) % U32
    return TONE_LUT[(phases >> 24).astype(np.intp)]


def tone_period(delta, chunk):
    """Chunks until the content repeats (to within a LUT step)."""
    first = tone_chunk(delta, 0, chunk).astype(int)
    for p in range(1, 1000):
        if np.abs(tone_chunk(delta, p, chunk).astype(int) - first).max() <= MATCH_TOL:
            return p
    raise ValueError("tone does not repeat within 1000 chunks")


def s32(x):
    """Signed difference of two uint32 clock readings."""
    return ((x + (1 << 31)) % U32) - (1 << 31)


class SlotStats:
    def __init__(self):
        self.placed = 0
        self.dup = 0
        self.unmatched = 0
        self.slips = 0
        self.ctr_disagree = 0
        self.gaps = {"radio": [0, 0], "log": [0, 0]}	# [count, frames]


Chunk = namedtuple("Chunk", "frame slot payload ctr brk")


# Trust threshold for a run of records (see trusted_records): twice the
# largest SD hole seen, 128 records.
TRUST_RUN = 256


def trusted_records(recs):
    """Drop the SD card's 8 KB hole (CHANGELOG, Known limitations).

    In a healthy file every record's seq - index is one constant, stepping up
    only where the on-device ring dropped records. A hole replaces a sector-
    aligned stretch with zeros, 0xFF, random bytes or stale sectors of other
    runs -- and random bytes can decode as a plausible RX record, so no
    per-record check is safe. Instead, split the file into runs of constant
    seq - index and trust:
      - the run holding record 0 (META) and every run of >= TRUST_RUN
        records, as long as offset and uptime never go backwards;
      - a shorter run between two trusted ones whose offset and uptime lie
        between theirs (a ring drop between two holes, say);
      - a short final run with the last trusted offset (good records after
        a hole near the end of the file), or with a larger one, if it holds
        at least two valid record types and uptime keeps rising (a ring or
        RTT drop near the end of the file).
    Returns the trusted records in file order and the count discarded.
    """
    runs = []	# [start, end, offset]
    for r in recs:
        off = r.seq - r.index
        if runs and runs[-1][2] == off:
            runs[-1][1] = r.index + 1
        else:
            runs.append([r.index, r.index + 1, off])

    def up(i):
        return recs[i].uptime_ms

    def rising(a, b):
        return all(up(i) <= up(i + 1) for i in range(a, b - 1))

    anchors = []
    for k, (a, b, off) in enumerate(runs):
        if a != 0 and b - a < TRUST_RUN:
            continue
        if anchors:
            pa, pb, poff = runs[anchors[-1]]
            if off < poff or up(a) < up(pb - 1):
                continue
        anchors.append(k)

    keep = set(anchors)
    for k, (a, b, off) in enumerate(runs):
        if k in keep or not rising(a, b):
            continue
        prev = [p for p in anchors if p < k]
        nxt = [n for n in anchors if n > k]
        if not prev:
            continue
        pa, pb, poff = runs[prev[-1]]
        if nxt:
            na, nb, noff = runs[nxt[0]]
            if poff <= off <= noff and up(pb - 1) <= up(a) and up(b - 1) <= up(na):
                keep.add(k)
        elif off == poff and up(pb - 1) <= up(a):
            keep.add(k)
        elif (off > poff and b - a >= 2 and up(pb - 1) <= up(a) and
              all(1 <= recs[i].type <= 4 for i in range(a, b))):
            # A ring or RTT drop near the end: seq steps forward, and what
            # follows is records of this run, not a hole's bytes.
            keep.add(k)

    good = [r for k in sorted(keep) for r in recs[runs[k][0]:runs[k][1]]]
    return good, len(recs) - len(good)


def clean_rx(recs, own_slot, slot_count):
    """Log hygiene: the RX records that belong to this run.

    Every discarded stretch and every seq jump (ring drop) is a log break, so
    a gap spanning one is blamed on the log, not the radio. Returns
    [(rec, breaks_so_far)] and the hygiene tallies.
    """
    good, discarded = trusted_records(recs)
    out = []
    breaks = 0
    prev = None
    tally = {"discarded": discarded, "missing": 0, "own_slot": 0}
    for r in good:
        if prev is not None and (r.index != prev.index + 1 or
                                 r.seq != prev.seq + 1):
            breaks += 1
            tally["missing"] += r.seq - prev.seq - 1
        prev = r
        if r.type != REC_RX:
            continue
        if r.slot_id == own_slot or r.slot_id >= slot_count:
            tally["own_slot"] += 1	# a unit cannot hear its own slot
            continue
        out.append((r, breaks))
    return out, tally


def frame_period(rx, slot_us, frame_us):
    """Median spacing of consecutive same-slot packets near one frame.

    Measured on the receiver's own clock, so it absorbs that crystal's error
    when stepping across long outages.
    """
    last = {}
    spans = []
    for r, _ in rx:
        if r.slot_id in last:
            d = s32(r.t_us - last[r.slot_id])
            if 0.5 * frame_us < d < 1.5 * frame_us:
                spans.append(d)
        last[r.slot_id] = r.t_us
    return float(np.median(spans)) if spans else float(frame_us)


def place(rx, slot_us, period):
    """Assign every packet a frame index on one shared timeline.

    tau = t_us - slot_id * slot_us removes the slot's offset in the frame, so
    all packets of one frame share a tau. t_us is the free-running 1 MHz slot
    clock (sync corrections never step it), so walking the records in order
    and rounding each tau step to whole frames is exact; the mod-2^32 step
    carries it across the counter's 71.6 min wrap.
    """
    frame = 0
    tau_prev = None
    out = []
    for r, brk in rx:
        tau = (r.t_us - r.slot_id * slot_us) % U32
        if tau_prev is not None:
            frame += int(round(s32(tau - tau_prev) / period))
        tau_prev = tau
        out.append(Chunk(frame, r.slot_id, np.frombuffer(r.payload, np.uint8),
                         r.frame_ctr, brk))
    return out


def analyse_slot(chunks, delta, period_chunks, chunk_len):
    """Accept, dedupe and check one peer's chunks. Returns (accepted, stats)."""
    st = SlotStats()
    accepted = []		# (frame, payload)
    prev = None		# last accepted Chunk
    anchor = None		# (frame, content index) of the last matched chunk
    expected = [tone_chunk(delta, k, chunk_len).astype(int)
                for k in range(period_chunks)]

    for c in chunks:
        if prev is not None:
            g = c.frame - prev.frame
            # Duplicates: a second record in the same frame, or a byte-
            # identical repeat one frame on (a stale re-transmit). Content
            # repeats every period_chunks, so identical chunks further apart
            # are legitimate.
            if g <= 0 or (g == 1 and np.array_equal(c.payload, prev.payload)):
                st.dup += 1
                continue
            if g > 1:
                kind = "log" if c.brk != prev.brk else "radio"
                st.gaps[kind][0] += 1
                st.gaps[kind][1] += g - 1
            if (c.ctr - prev.ctr) % 0x10000 != g:
                st.ctr_disagree += 1

        accepted.append((c.frame, c.payload))
        st.placed += 1
        prev = c

        # Content check: which chunk (mod the period) is this, and does it
        # advance with the frame count?
        got = c.payload.astype(int)
        dist = [int(np.abs(got - e).max()) for e in expected]
        k = int(np.argmin(dist))
        if dist[k] > MATCH_TOL:
            st.unmatched += 1
            continue
        if anchor is not None:
            g = c.frame - anchor[0]
            if g == 1 and (k - anchor[1]) % period_chunks != 1:
                st.slips += 1
        anchor = (c.frame, k)

    return accepted, st


def spectral_peaks(x, fs, count=6, floor_db=-20.0):
    """Strongest spectral peaks of x, as [(Hz, dB re the strongest)]."""
    if len(x) < 64:
        return []
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    freqs = np.fft.rfftfreq(len(x), 1.0 / fs)
    top = spec.max()
    if top == 0:
        return []
    is_peak = np.r_[False, (spec[1:-1] > spec[:-2]) & (spec[1:-1] >= spec[2:]), False]
    idx = [i for i in np.flatnonzero(is_peak)
           if 20 * math.log10(spec[i] / top) >= floor_db]
    idx.sort(key=lambda i: -spec[i])
    peaks = []
    for i in idx:
        if all(abs(freqs[i] - f) > 5.0 for f, _ in peaks):
            peaks.append((float(freqs[i]), 20 * math.log10(spec[i] / top)))
        if len(peaks) == count:
            break
    return peaks


def write_wav(path, samples_i16, fs):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(samples_i16.astype("<i2").tobytes())


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile")
    ap.add_argument("-o", "--out", help="mix WAV (default: next to the log)")
    ap.add_argument("--per-slot", action="store_true",
                    help="also write one WAV per peer slot")
    args = ap.parse_args()

    blob = open(args.logfile, "rb").read()
    recs = list(iter_records(blob))
    meta = next((parse_meta(r.payload) for r in recs if r.type == REC_META),
                None)
    if meta is None:
        sys.exit("%s: no valid META record, cannot tell the payload type"
                 % args.logfile)
    if meta["payload_mode"] != PAYLOAD_TONE:
        sys.exit("%s: not a tone log (%s); rebuild the units with "
                 "CONFIG_SOAK_PAYLOAD_TONE=y" % (args.logfile, payload_desc(meta)))

    fs = meta["tone_fs_khz"] * 1000
    f0 = meta["tone_f0_hz"]
    chunk_len = meta["payload_len"]
    slot_us, frame_us = meta["slot_us"], meta["frame_us"]
    own = meta["slot_id"]
    audio_ms_per_frame = 1000.0 * chunk_len / fs
    air_ms_per_frame = frame_us / 1000.0

    rx, skipped = clean_rx(recs, own, meta["slot_count"])
    if not rx:
        sys.exit("%s: no usable RX records" % args.logfile)
    period = frame_period(rx, slot_us, frame_us)
    chunks = place(rx, slot_us, period)

    first = min(c.frame for c in chunks)
    n_frames = max(c.frame for c in chunks) - first + 1
    slots = sorted({c.slot for c in chunks})

    print("file:     %s" % args.logfile)
    print("logger:   %s slot=%d %s" % (meta["role"], own, payload_desc(meta)))
    print("hygiene:  %d records discarded (SD hole), %d missing from the"
          " sequence (hole + ring drops), %d own-slot RX ignored" % (
              skipped["discarded"], skipped["missing"], skipped["own_slot"]))
    print("timeline: %d frames = %.1f s on air -> %.2f s of audio (%.0fx);"
          " frame period %.1f us" % (
              n_frames, n_frames * air_ms_per_frame / 1000.0,
              n_frames * audio_ms_per_frame / 1000.0,
              air_ms_per_frame / audio_ms_per_frame, period))

    streams = {}
    for s in slots:
        f = f0 * (s + 1)
        delta = tone_delta(f, fs)
        p = tone_period(delta, chunk_len)
        accepted, st = analyse_slot([c for c in chunks if c.slot == s], delta,
                                    p, chunk_len)
        stream = np.full(n_frames * chunk_len, TONE_SILENCE, dtype=np.uint8)
        for frame, payload in accepted:
            i = (frame - first) * chunk_len
            stream[i:i + chunk_len] = payload
        streams[s] = stream

        def gap_str(kind):
            n, fr = st.gaps[kind]
            return "%d (%d frames = %.0f ms audio / %.2f s air)" % (
                n, fr, fr * audio_ms_per_frame, fr * air_ms_per_frame / 1000.0)

        print("slot %d   %4d Hz: placed %d  dup %d  unmatched %d  slips %d" % (
            s, f, st.placed, st.dup, st.unmatched, st.slips))
        print("           gaps: radio %s, log %s" % (
            gap_str("radio"), gap_str("log")))
        print("           frame_ctr disagrees with arrival time at %d boundaries"
              % st.ctr_disagree)

    # int16, silence at 0; peers summed with 1/N scaling.
    pcm = {s: (v.astype(np.int32) - TONE_SILENCE) * 256 for s, v in streams.items()}
    mix = (sum(pcm.values()) // len(pcm)).astype(np.int16)

    out = args.out or os.path.splitext(args.logfile)[0] + ".wav"
    write_wav(out, mix, fs)
    print("wrote:    %s (%d samples, %.2f s)" % (out, len(mix), len(mix) / fs))
    if args.per_slot:
        stem, ext = os.path.splitext(out)
        for s, v in pcm.items():
            path = "%s_slot%d%s" % (stem, s, ext or ".wav")
            write_wav(path, v.astype(np.int16), fs)
            print("wrote:    %s" % path)

    peaks = spectral_peaks(mix.astype(float), fs)
    want = [f0 * (s + 1) for s in slots]
    print("peaks:    " + ", ".join("%.0f Hz (%.1f dB)" % pk for pk in peaks))
    missing = [f for f in want if not any(abs(f - pf) <= 2 for pf, _ in peaks)]
    extra = [pf for pf, _ in peaks if not any(abs(f - pf) <= 2 for f in want)]
    verdict = "OK" if not missing and not extra else \
        "missing %s, extra %s" % (missing or "-", ["%.0f" % e for e in extra] or "-")
    print("expected: %s Hz -> %s" % (", ".join(str(f) for f in want), verdict))


if __name__ == "__main__":
    main()
