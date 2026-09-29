# Codec 2 (vendored subset)

The Codec 2 speech codec by David Rowe and contributors, from https://github.com/drowe67/codec2 at 1.2.0 (commit `06d4c11e699b0351765f10398abb4f663a984f36`). LGPL-2.1: see [`COPYING`](COPYING).

Only the codec itself is here: the sources in `scripts/vendor_codec2.py` (`SOURCES`) and the headers they include, unmodified, plus the codebook tables generated from upstream's `src/codebook/*.txt` by that script's Python port of `generate_codebook.c`, and `include/codec2/version.h` configured from upstream's `cmake/version.h.in`. Regenerate with:

```sh
git clone --branch 1.2.0 https://github.com/drowe67/codec2.git /tmp/codec2
python scripts/vendor_codec2.py /tmp/codec2
```

Linked only into builds with `CONFIG_SOAK_C2_ENCODE` (test firmware); see `docs/c2_encode_test.md`.

## Patches (Project-Blue-Frog)

Applied in order from `external/codec2-patches/` by the script. Each changed spot is marked `Project-Blue-Frog patch` in the source. Measured on the nRF5340 at 128 MHz (docs/live_audio_wm8960.md, G0):

- `0001-nlp-decimating-fir.patch`: `nlp()`'s 48-tap decimation FIR keeps its memory in a mirrored circular buffer and computes an output only where the decimation reads one (every 5th sample). Encoded frames are bit-identical to upstream (372 chunks compared on the device). Encode -7.2 ms per 80 ms chunk.
- `0002-quantise-direct-lpc-spectrum-and-postfilter-pow.patch`: `aks_to_M2()` evaluates the order-10 LPC spectrum and the post-filter's weighting spectrum directly in one pass (11 terms at 257 bins, cosine table) instead of two 512-point real FFTs (within 3.4e-7 of the FFT, relative to the spectrum peak); `lpc_post_filter()` takes that spectrum and uses a dedicated positive-argument power function instead of `powf` (max relative error 1.4e-6). Decode output differs from upstream only at float-rounding level. Decode -8.4 ms per chunk.
