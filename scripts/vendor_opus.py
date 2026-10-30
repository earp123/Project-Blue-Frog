#!/usr/bin/env python3
"""Copy the libopus subset this tree builds into external/opus/, unmodified.

    python scripts/vendor_opus.py path/to/opus-1.5.2

The source is an extracted release tarball (opus-1.5.2.tar.gz, sha256
65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1 as
published in xiph's SHA256SUMS.txt). The file lists are upstream's own:
CELT_SOURCES, SILK_SOURCES, SILK_SOURCES_FIXED and OPUS_SOURCES from the
.mk files, every header beside them, include/, and COPYING. Left out: the
float-API analysis (OPUS_SOURCES_FLOAT; the build is DISABLE_FLOAT_API),
the per-architecture directories, demos, tests and dnn/ (no deep PLC, DRED
or OSCE).
"""

import os
import re
import shutil
import sys

LISTS = {"celt_sources.mk": ["CELT_SOURCES"],
         "silk_sources.mk": ["SILK_SOURCES", "SILK_SOURCES_FIXED"],
         "opus_sources.mk": ["OPUS_SOURCES"]}
HEADER_DIRS = ["include", "celt", "silk", "silk/fixed", "src"]


def mk_list(path, name):
    text = open(path).read().replace("\\\n", " ")
    m = re.search(r"^%s\s*=(.*)$" % name, text, re.M)
    if not m:
        sys.exit("%s: no %s" % (path, name))
    return m.group(1).split()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                       "external", "opus")
    files = ["COPYING"]
    for mk, names in LISTS.items():
        for name in names:
            files += mk_list(os.path.join(src, mk), name)
    for d in HEADER_DIRS:
        files += [os.path.join(d, f) for f in sorted(os.listdir(
            os.path.join(src, d))) if f.endswith(".h")]
    for keep in [os.path.join(dst, "README.md")]:
        if os.path.exists(keep):
            readme = open(keep, "rb").read()
            break
    else:
        readme = None
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    for f in files:
        os.makedirs(os.path.join(dst, os.path.dirname(f)), exist_ok=True)
        shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))
    if readme is not None:
        open(os.path.join(dst, "README.md"), "wb").write(readme)
    print("%d files -> %s" % (len(files), os.path.normpath(dst)))


if __name__ == "__main__":
    main()
