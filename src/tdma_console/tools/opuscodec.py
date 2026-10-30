"""Opus on the host, for opus_c0.py and (later) reconstruct_opus.py.

A ctypes binding to libopus, no compiler: $OPUS_LIB if set, else the system
library (brew install opus, apt install libopus0), else the opus.dll that
PyOgg's Windows wheel carries (pip install pyogg). That DLL is a 1.3.x build
(no version string; the projection API but none of 1.5's DRED); the device
vendors 1.5.x (docs/opus_payload.md decision 1).

The encoder settings are decision 2's: SILK narrowband, VOIP, voice, CBR,
no DTX, no in-band FEC, so every 20 ms frame is exactly bitrate * 20 ms / 8
bytes (18 B at 7200). Plus SILK forced (OPUS_SET_FORCE_MODE, a private ctl
from opus_private.h): left to choose, libopus codes a 20 ms frame of under
15 B (6 kbit/s) as CELT, so 14 B frames would silently stop being SILK. One
encoder state per stream and one decoder per peer, as on the device. 8 kHz,
16-bit signed mono.
"""

import ctypes
import ctypes.util
import os

import numpy as np

FS = 8000
FRAME_SAMPLES = 160		# 20 ms

_APPLICATION_VOIP = 2048
_SET_BITRATE = 4002
_SET_VBR = 4006
_SET_BANDWIDTH = 4008
_SET_COMPLEXITY = 4010
_SET_INBAND_FEC = 4012
_SET_DTX = 4016
_SET_SIGNAL = 4024
_GET_LOOKAHEAD = 4027
_SET_FORCE_MODE = 11002		# opus_private.h
_BANDWIDTH_NARROWBAND = 1101
_SIGNAL_VOICE = 3001
_MODE_SILK_ONLY = 1000
_AUTO = -1000

_lib = None
_lib_path = None


def _candidates():
    if os.environ.get("OPUS_LIB"):
        yield os.environ["OPUS_LIB"]
    p = ctypes.util.find_library("opus")
    if p:
        yield p
    for p in ("/opt/homebrew/lib/libopus.dylib", "/usr/local/lib/libopus.dylib"):
        yield p
    try:
        import pyogg
        yield os.path.join(os.path.dirname(pyogg.__file__), "opus.dll")
    except ImportError:
        pass


def lib():
    global _lib, _lib_path
    if _lib is not None:
        return _lib
    for p in _candidates():
        try:
            _lib = ctypes.CDLL(p)
        except OSError:
            continue
        _lib_path = p
        break
    else:
        raise SystemExit("no libopus: set OPUS_LIB, brew install opus / apt "
                         "install libopus0, or pip install pyogg (Windows)")
    L = _lib
    P = ctypes.c_void_p
    L.opus_get_version_string.restype = ctypes.c_char_p
    L.opus_encoder_create.restype = P
    L.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int,
                                      ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    L.opus_encode.restype = ctypes.c_int32
    L.opus_encode.argtypes = [P, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                              ctypes.c_char_p, ctypes.c_int32]
    L.opus_encoder_destroy.argtypes = [P]
    L.opus_encoder_ctl.restype = ctypes.c_int
    L.opus_decoder_create.restype = P
    L.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_int)]
    L.opus_decode.restype = ctypes.c_int
    L.opus_decode.argtypes = [P, ctypes.c_char_p, ctypes.c_int32,
                              ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                              ctypes.c_int]
    L.opus_decoder_destroy.argtypes = [P]
    return L


def backend():
    """Library path and version string; raises if there is no libopus."""
    v = lib().opus_get_version_string().decode()
    return "%s (%s)" % (v, os.path.basename(_lib_path))


def frame_bytes(bitrate):
    return bitrate * FRAME_SAMPLES // FS // 8


def toc_config(packet):
    """TOC config number: 1 is SILK-only, narrowband, 20 ms."""
    return packet[0] >> 3


class Encoder:
    def __init__(self, bitrate=7200, complexity=2, silk=True):
        """silk=False leaves the mode to libopus (CELT under 15 B)."""
        L = lib()
        err = ctypes.c_int()
        self.st = L.opus_encoder_create(FS, 1, _APPLICATION_VOIP,
                                        ctypes.byref(err))
        if err.value != 0 or not self.st:
            raise RuntimeError("opus_encoder_create: %d" % err.value)
        self.nbytes = frame_bytes(bitrate)
        for req, val in ((_SET_BANDWIDTH, _BANDWIDTH_NARROWBAND),
                         (_SET_SIGNAL, _SIGNAL_VOICE), (_SET_VBR, 0),
                         (_SET_BITRATE, bitrate), (_SET_DTX, 0),
                         (_SET_INBAND_FEC, 0),
                         (_SET_COMPLEXITY, complexity),
                         (_SET_FORCE_MODE, _MODE_SILK_ONLY if silk else _AUTO)):
            rc = L.opus_encoder_ctl(ctypes.c_void_p(self.st), ctypes.c_int(req),
                                    ctypes.c_int(val))
            if rc != 0:
                raise RuntimeError("opus_encoder_ctl(%d, %d): %d" % (req, val, rc))

    def lookahead(self):
        v = ctypes.c_int()
        lib().opus_encoder_ctl(ctypes.c_void_p(self.st),
                               ctypes.c_int(_GET_LOOKAHEAD), ctypes.byref(v))
        return v.value

    def encode(self, frame):
        """160 int16 samples -> one packet, exactly self.nbytes long."""
        pcm = np.ascontiguousarray(frame, dtype=np.int16)
        out = ctypes.create_string_buffer(256)
        n = lib().opus_encode(self.st, pcm.ctypes.data_as(
            ctypes.POINTER(ctypes.c_int16)), FRAME_SAMPLES, out, len(out))
        if n != self.nbytes:
            raise RuntimeError("opus_encode gave %d B, CBR wants %d"
                               % (n, self.nbytes))
        return out.raw[:n]

    def __del__(self):
        if getattr(self, "st", None):
            lib().opus_encoder_destroy(self.st)
            self.st = None


class Decoder:
    def __init__(self):
        err = ctypes.c_int()
        self.st = lib().opus_decoder_create(FS, 1, ctypes.byref(err))
        if err.value != 0 or not self.st:
            raise RuntimeError("opus_decoder_create: %d" % err.value)

    def decode(self, packet):
        """One packet -> 160 int16; None runs the decoder's PLC instead."""
        pcm = np.zeros(FRAME_SAMPLES, dtype=np.int16)
        n = lib().opus_decode(self.st, packet, len(packet) if packet else 0,
                              pcm.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
                              FRAME_SAMPLES, 0)
        if n != FRAME_SAMPLES:
            raise RuntimeError("opus_decode: %d" % n)
        return pcm

    def __del__(self):
        if getattr(self, "st", None):
            lib().opus_decoder_destroy(self.st)
            self.st = None


PLC_MAX = 2		# chunks of PLC played before silence (decision 3)


def decode_chunks(chunks, frames=4, plc_max=PLC_MAX):
    """One peer's playout: chunks in index order, each that chunk's bytes
    (frames equal packets) or None if it was lost. One decoder state through
    the stream. A lost chunk runs the PLC for its frames, played for up to
    plc_max chunks in a row and silence after, the decoder still running so
    its state follows real time (docs/opus_payload.md decision 3); plc_max=0
    plays silence for every lost chunk. Returns int16 PCM."""
    dec = Decoder()
    n = frames * FRAME_SAMPLES
    out = np.zeros(len(chunks) * n, np.int16)
    run = 0
    for k, c in enumerate(chunks):
        if c is None:
            packets = [None] * frames
            run += 1
        else:
            fb = len(c) // frames
            packets = [c[i * fb:(i + 1) * fb] for i in range(frames)]
            run = 0
        pcm = np.concatenate([dec.decode(p) for p in packets])
        if run <= plc_max:
            out[k * n:(k + 1) * n] = pcm
    return out


def encode(pcm, bitrate=7200, complexity=2, silk=True):
    """int16 PCM -> list of packets, one per 20 ms frame, one encoder state.
    A trailing partial frame is dropped."""
    pcm = np.asarray(pcm, dtype=np.int16)
    enc = Encoder(bitrate, complexity, silk)
    return [enc.encode(pcm[i:i + FRAME_SAMPLES])
            for i in range(0, len(pcm) // FRAME_SAMPLES * FRAME_SAMPLES,
                           FRAME_SAMPLES)]
