# libopus (vendored subset)

The Opus codec by the Xiph.Org Foundation and contributors, release 1.5.2: `opus-1.5.2.tar.gz`, sha256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`, matching xiph's published `SHA256SUMS.txt`. BSD-3-Clause, plus the patent licences named in [`COPYING`](COPYING). Unlike Codec 2 (LGPL), the licence is fine for the product.

Unmodified upstream files, chosen by upstream's own lists:

- `celt/`: `CELT_SOURCES` (`celt_sources.mk`) and every header beside them.
- `silk/`, `silk/fixed/`: `SILK_SOURCES` and `SILK_SOURCES_FIXED` (`silk_sources.mk`) and their headers.
- `src/`: `OPUS_SOURCES` (`opus_sources.mk`) and its headers. Not `OPUS_SOURCES_FLOAT` (the float-API analysis), the demos or the tests.
- `include/`: the public headers.

Nothing from `dnn/` (deep PLC, DRED, OSCE): those stay off, and every reference to them in these files is behind their `ENABLE_*` guards. Re-vendor with `python scripts/vendor_opus.py <extracted opus-x.y.z>`, which keeps this README.

## Build (Project-Blue-Frog)

Linked only into builds with `CONFIG_SOAK_OPUS_DEC` (see `CMakeLists.txt`): a static library, `FIXED_POINT`, `DISABLE_FLOAT_API`, `VAR_ARRAYS` (scratch on the calling thread's stack: `src/tdma_console/opus_dec.c` sizes it and reports the high-water mark), `-O2` through `external/codec2_opt.h`, warnings off. No allocation: decoder states live in static buffers, set up with `opus_decoder_init()`. See `docs/opus_payload.md`.
