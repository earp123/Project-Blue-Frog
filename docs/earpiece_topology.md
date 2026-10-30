# Earpiece topology: digitize at the ear

**Status:** decision, 2026-10-30. Not a coding task yet; the firmware work
that follows it is the VPU bench gate below. **Context:** the earpiece
design block of 2026-09-29 (boom mic + secondary mic + VPU as the VOX gate,
off-spec USB-C plug) left the cable topology open — analog lines to a
codec on the main board, or digital at the ear.

## Decision

**Mics and the VPU convert to digital inside the earpiece.** The cable
carries PDM and the VPU's bus, not microphone audio. The main board keeps
no codec ADC; it needs only a DAC / headphone driver for the earphone.

```
earpiece [PDM mic (boom)] [PDM mic (noise ref)] [VPU] [earphone]
   ║ PDM CLK, PDM DATA (2 mics on one line), VPU bus, earphone, 3V3|1V8, GND
   ▼
main board  nRF5340 PDM peripheral ── encoder          DAC + HP amp ◀── playout
```

**Why:**
- The TDMA frame is 80 ms = **12.5 Hz**, the GSM-buzz frequency, and the
  mic lines would run ~60 cm past a +22 dBm PA at 915 MHz. Digital mics at
  the ear remove that whole class of problem; analog mic lines would have
  to be proven clean on every cable and body position.
- The nRF5340 PDM peripheral takes two mics on one data line natively, so
  the boom mic and the noise-reference mic cost one clock and one data pin
  and no codec.
- Power is one rail: PDM MEMS mics (1.6–3.6 V, ~1 mA each), the VPU
  (LIS25BA 1.7–3.6 V; Sonion analog ~1.2–3.3 V) and a small DAC/HP amp if
  the earphone goes digital (1.8–3.3 V, 5–10 mA) all run from 3V3, or from
  1V8 on the Audio DK. Nothing regulates inside the earpiece.
- The WM8960 HAT stays a bench part. The codec work in `opus_payload.md` is
  unaffected: the encoder takes 20 ms 8 kHz frames from `hat_audio_frame()`
  today and will take them from a PDM capture ring tomorrow.

## Fixed by this decision

- Plug carries: PDM CLK, PDM DATA, VPU bus (2–4 lines, see below),
  earphone (1–2), supply, GND → **~8–10 lines**, i.e. a **custom cable** on
  the off-spec USB-C plug (an off-the-shelf USB 2.0 C-C cable carries no
  SBU lines). The ID team can start the pinout from this count.
- Supply pin level follows the host GPIO (3V3 on the regular DK, 1V8 on the
  Audio DK); every earpiece part is chosen to run at either.
- At the earpiece: 1 µF + 100 nF per device, a ferrite on the supply, TVS
  on every plug pin; a few ohms or a soft-start on the supply at the main
  board for hot-plug. Firmware must tolerate PDM data stopping mid-run, as
  the HAT path already tolerates I2S restarts.

## Open — in order

1. **VPU bench gate (first; blocks the pinout's last lines).** Sonion
   (analog) vs ST LIS25BA (digital TDM) for discrimination of the wearer's
   voice under crowd SPL. Also to check: whether the LIS25BA's TDM frame
   can be received by the nRF5340 I2S peripheral as-is, or needs a second
   interface. An analog VPU adds one line into the SAADC and is fine on a
   custom cable.
2. **Earphone: analog over the cable (main-board DAC/HP amp) or a DAC at
   the ear (I2S over the cable, +2–3 lines).** Analog is the default until
   RF pickup on the earphone line is shown to matter; it is a low-impedance
   driven line, unlike a mic input.
3. **Parts.** PDM mics: any 1.6–3.6 V bottom-port MEMS pair with matched
   sensitivity (Knowles / TDK / Infineon); DAC/HP amp candidates
   TLV320DAC3100 class; keep the CS47L63 (Audio DK codec, driver exists in
   `nrf5340_audio`) as the smart-codec alternative only if its sourcing
   and NDA are acceptable.
4. **A `pdm_audio` capture source** replacing `hat_audio` on the product
   board: same ring and `*_frame()` contract, so the encoder and the mic
   DSP front end do not change.
