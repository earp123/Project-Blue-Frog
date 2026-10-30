"""Live earpiece monitor on the PC: what one unit hears, played as it arrives.

Used by `rtt_link.py capture --play SLOT` (one unit) or `rtt_link.py --all
capture --play SLOT` (every unit, keys 0-3 pick whose ears you hear, m
mutes). The capture is unchanged: every record still goes to the file; the
monitor reads the same bytes on the side.

Each RX record carrying a clip chunk (clip_src.h header: magic 0xC2, codec 0
= Codec 2 3200, 1 = Opus) feeds that sender's decoder, persistent per sender
as on a unit. A chunk-index gap is concealed as decision 3 of
docs/opus_payload.md plays it: Opus PLC for up to two chunks, silence after;
Codec 2 has no PLC, so its gaps are silence, as today. Senders are mixed with
saturation and played at 8 kHz from a small jitter buffer per sender
(~240 ms), like hat_audio.c's playout ring. Nothing here is device code; it
is a bench ear for units without a HAT.
"""

import struct
import threading
from collections import deque

import numpy as np

import opuscodec

FS = 8000
CHUNK = 640			# samples per 80 ms chunk
FRAMES = 4
HDR = 8
MAGIC = 0xC2
REC_RX = 2
TARGET = 3 * CHUNK		# jitter buffer fill before a sender plays
HIGH = TARGET + 2 * CHUNK	# above this the oldest audio is dropped
GAP_RESET = 25			# a gap this long (2 s) is an outage: new decoder


def s16(x):
    return ((x + 0x8000) & 0xFFFF) - 0x8000


class Peer:
    def __init__(self, codec):
        self.codec = codec
        self.dec = self._decoder()
        self.top = None			# highest unwrapped chunk index seen
        self.run = 0			# lost chunks in a row
        self.buf = deque()		# int16 arrays, oldest first
        self.level = 0			# samples in buf
        self.playing = False
        self.got = self.lost = self.plc = self.under = self.dropped = 0

    def _decoder(self):
        if self.codec == 1:
            return opuscodec.Decoder()
        import pycodec2
        return pycodec2.Codec2(3200)

    def _decode(self, body):
        """One chunk (bytes, or None if lost) -> 640 int16 samples."""
        if self.codec == 1:
            if body is None:
                pcm = np.concatenate([self.dec.decode(None)
                                      for _ in range(FRAMES)])
            else:
                fb = len(body) // FRAMES
                pcm = np.concatenate([self.dec.decode(body[i * fb:(i + 1) * fb])
                                      for i in range(FRAMES)])
            return pcm if body is not None or self.run <= opuscodec.PLC_MAX \
                else np.zeros(CHUNK, np.int16)
        if body is None:
            return np.zeros(CHUNK, np.int16)	# Codec 2: no concealment
        return np.concatenate([np.asarray(self.dec.decode(body[i * 8:(i + 1) * 8]),
                                          np.int16) for i in range(FRAMES)])

    def chunk(self, idx16, body):
        """Decoded audio for this chunk and any gap before it, in order."""
        if self.top is None:
            u = idx16
        else:
            u = self.top + s16(idx16 - (self.top & 0xFFFF))
            if u <= self.top:
                return []			# duplicate or late
        out = []
        if self.top is not None and u - self.top > 1:
            missing = u - self.top - 1
            if missing >= GAP_RESET:
                self.dec = self._decoder()
                self.lost += missing
            else:
                for _ in range(missing):
                    self.run += 1
                    self.lost += 1
                    if self.codec == 1 and self.run <= opuscodec.PLC_MAX:
                        self.plc += 1
                    out.append(self._decode(None))
        self.run = 0
        self.got += 1
        out.append(self._decode(body))
        self.top = u
        return out


class Monitor:
    def __init__(self, own_slot, label=""):
        self.own = own_slot
        self.label = label
        self.peers = {}
        self.pending = b""
        self.size = None
        self.lock = threading.Lock()
        self.stream = None
        self.active = False

    # ---- record side (capture thread) ----------------------------------
    def feed(self, data):
        self.pending += data
        if self.size is None:
            if len(self.pending) < 64:
                return
            # Record 0 is META; its payload_len (record offset 54) picks
            # the record size, as decode_soak_log.rec_layout() does.
            self.size = 128 if (self.pending[0] == 1 and
                                self.pending[54] > 40) else 64
        size = self.size
        plen = 80 if size == 128 else 40
        whole = len(self.pending) // size * size
        for off in range(0, whole, size):
            rec = self.pending[off:off + size]
            if rec[0] != REC_RX:
                continue
            slot = rec[1]
            p = rec[24:24 + plen]
            if slot == self.own or p[0] != MAGIC or p[1] not in (0, 1):
                continue
            (idx16,) = struct.unpack_from("<H", p, 2)
            peer = self.peers.get(slot)
            if peer is None or peer.codec != p[1]:
                peer = self.peers[slot] = Peer(p[1])
            pcms = peer.chunk(idx16, bytes(p[HDR:]))
            with self.lock:
                for pcm in pcms:
                    peer.buf.append(pcm)
                    peer.level += len(pcm)
                while peer.level > HIGH:
                    peer.level -= len(peer.buf.popleft())
                    peer.dropped += 1
        self.pending = self.pending[whole:]

    # ---- audio side -----------------------------------------------------
    def _take(self, peer, n):
        out = np.zeros(n, np.int32)
        if not peer.playing:
            if peer.level < TARGET:
                return out
            peer.playing = True
        got = 0
        while got < n and peer.buf:
            head = peer.buf[0]
            k = min(n - got, len(head))
            out[got:got + k] = head[:k]
            got += k
            if k == len(head):
                peer.buf.popleft()
            else:
                peer.buf[0] = head[k:]
            peer.level -= k
        if got < n:
            peer.under += 1
            peer.playing = False		# refill before playing again
        return out

    def _callback(self, outdata, frames, time_info, status):
        with self.lock:
            mix = np.zeros(frames, np.int32)
            for peer in self.peers.values():
                mix += self._take(peer, frames)
        outdata[:, 0] = np.clip(mix, -32768, 32767).astype(np.int16)

    def set_active(self, on):
        if on == self.active:
            return
        self.active = on
        if on:
            import sounddevice as sd
            with self.lock:		# start from a fresh jitter buffer
                for peer in self.peers.values():
                    peer.buf.clear()
                    peer.level = 0
                    peer.playing = False
            self.stream = sd.OutputStream(samplerate=FS, channels=1,
                                          dtype="int16", blocksize=160,
                                          callback=self._callback)
            self.stream.start()
        else:
            self.stream.stop()
            self.stream.close()
            self.stream = None

    def status(self):
        """One line per report: per sender received / lost (PLC) / under."""
        return "  ".join(
            "from %d %s: %d rx, %d lost (%d PLC), %d underruns" % (
                s, "opus" if p.codec == 1 else "c2", p.got, p.lost, p.plc,
                p.under)
            for s, p in sorted(self.peers.items()))

    def close(self):
        self.set_active(False)
