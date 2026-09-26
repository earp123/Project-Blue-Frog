# Codec 2 (vendored subset)

The Codec 2 speech codec by David Rowe and contributors, from https://github.com/drowe67/codec2 at 1.2.0 (commit `06d4c11e699b0351765f10398abb4f663a984f36`). LGPL-2.1: see [`COPYING`](COPYING).

Only the codec itself is here: the sources in `scripts/vendor_codec2.py` (`SOURCES`) and the headers they include, unmodified, plus the codebook tables generated from upstream's `src/codebook/*.txt` by that script's Python port of `generate_codebook.c`, and `include/codec2/version.h` configured from upstream's `cmake/version.h.in`. Regenerate with:

```sh
git clone --branch 1.2.0 https://github.com/drowe67/codec2.git /tmp/codec2
python scripts/vendor_codec2.py /tmp/codec2
```

Linked only into builds with `CONFIG_SOAK_C2_ENCODE` (test firmware); see `docs/c2_encode_test.md`.
