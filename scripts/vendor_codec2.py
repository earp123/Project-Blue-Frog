#!/usr/bin/env python3
"""Vendor the Codec 2 speech codec into external/codec2/ for the firmware.

Usage:
    git clone --branch 1.2.0 https://github.com/drowe67/codec2.git /tmp/codec2
    python scripts/vendor_codec2.py /tmp/codec2

Copies the codec's own C sources (not the modems, FreeDV or LDPC code) plus
exactly the headers they include, the licence, and the codebook tables. Upstream
generates the tables at build time with a host C program (generate_codebook.c);
this script does the same in Python, so the firmware build needs no host
compiler. The output matches generate_codebook's: each table value is parsed as
a C float and printed with %g.

Codec 2 is LGPL-2.1 (external/codec2/COPYING). It is linked only into builds
with CONFIG_SOAK_C2_ENCODE (see Kconfig); the product licensing question is
open (docs/c2_encode_test.md).
"""

import math
import os
import re
import shutil
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "external", "codec2")

# The speech codec itself. Linked with --gc-sections, so modes this project
# never selects cost flash only for what codec2_create() references.
SOURCES = [
    "codec2.c", "codec2_fft.c", "kiss_fft.c", "kiss_fftr.c", "lpc.c", "lsp.c",
    "nlp.c", "phase.c", "postfilter.c", "quantise.c", "sine.c", "interp.c",
    "pack.c", "mbest.c", "newamp1.c",
]

# generate_codebook invocations from upstream src/CMakeLists.txt:
# output file, array name, input tables.
CODEBOOKS = [
    ("codebook.c", "lsp_cb", ["lsp%d.txt" % i for i in range(1, 11)]),
    ("codebookd.c", "lsp_cbd", ["dlsp%d.txt" % i for i in range(1, 11)]),
    ("codebookjmv.c", "lsp_cbjmv", ["lspjmv1.txt", "lspjmv2.txt",
                                    "lspjmv3.txt"]),
    ("codebookge.c", "ge_cb", ["gecb.txt"]),
    ("codebooknewamp1.c", "newamp1vq_cb", ["train_120_1.txt",
                                           "train_120_2.txt"]),
    ("codebooknewamp1_energy.c", "newamp1_energy_cb",
     ["newamp1_energy_q.txt"]),
    ("codebooknewamp2.c", "newamp2vq_cb", ["codes_450.txt"]),
    ("codebooknewamp2_energy.c", "newamp2_energy_cb",
     ["newamp2_energy_q.txt"]),
]

HEADER = ("/* THIS IS A GENERATED FILE. Edit generate_codebook.c and its input */"
          "\n\n/*\n * This intermediary file and the files that used to create "
          "it are under \n * The LGPL. See the file COPYING.\n */\n\n"
          "#include \"defines.h\"\n\n")

NUM = re.compile(r"[-.0-9][-+.0-9eE]*")


def c_float(x):
    """Round to C float, as strtod() into a float does."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def load(path):
    vals = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0]
            vals += [float(t) for t in NUM.findall(line)]
    k, m = int(vals[0]), int(vals[1])
    cb = [c_float(v) for v in vals[2:2 + k * m]]
    if len(cb) != k * m:
        sys.exit("%s: expected %d values, got %d" % (path, k * m, len(cb)))
    return k, m, cb


def gen(name, paths):
    out = [HEADER]
    books = [load(p) for p in paths]
    for i, (p, (k, m, cb)) in enumerate(zip(paths, books)):
        out.append("  /* %s */\n" % p)
        out.append("#ifdef __EMBEDDED__\nstatic const float codes%d[] = {\n"
                   "#else\nstatic float codes%d[] = {\n#endif\n" % (i, i))
        for j, v in enumerate(cb):
            out.append("  %g" % v)
            if j < len(cb) - 1:
                out.append(",")
            if (j + 1) % k == 0:
                out.append("\n")
        out.append("};\n")
    out.append("\nconst struct lsp_codebook %s[] = {\n" % name)
    for i, (p, (k, m, cb)) in enumerate(zip(paths, books)):
        log2m = int(round(math.log(m) / math.log(2)))
        out.append("  /* %s */\n  {\n    %d,\n    %d,\n    %d,\n    codes%d\n"
                   "  },\n" % (p, k, log2m, m, i))
    out.append("  { 0, 0, 0, 0 }\n};\n")
    return "".join(out)


def include_closure(src_dir, files):
    """The local headers the given sources include, transitively."""
    todo, seen = list(files), set()
    inc = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
    while todo:
        f = todo.pop()
        p = os.path.join(src_dir, f)
        if f in seen or not os.path.exists(p):
            continue
        seen.add(f)
        todo += inc.findall(open(p, encoding="latin-1").read())
    return sorted(h for h in seen if h.endswith(".h"))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    up = sys.argv[1]
    src = os.path.join(up, "src")
    commit = subprocess.run(["git", "-C", up, "rev-parse", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    tag = subprocess.run(["git", "-C", up, "describe", "--tags"],
                         capture_output=True, text=True).stdout.strip()

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, "src"))
    shutil.copy(os.path.join(up, "COPYING"), OUT)

    for f in SOURCES + include_closure(src, SOURCES):
        shutil.copy(os.path.join(src, f), os.path.join(OUT, "src", f))
    for out, name, tables in CODEBOOKS:
        paths = [os.path.join(src, "codebook", t) for t in tables]
        # load() needs the real paths; the comments carry upstream-relative
        # ones so the output does not depend on where the checkout lives.
        books = gen(name, paths)
        for p in paths:
            books = books.replace(p, os.path.relpath(p, up).replace(os.sep, "/"))
        with open(os.path.join(OUT, "src", out), "w", newline="\n") as f:
            f.write(books)

    # codec2.h includes <codec2/version.h>, which upstream's CMake configures
    # from cmake/version.h.in and the project() version.
    cm = open(os.path.join(up, "CMakeLists.txt")).read()
    ver = re.search(r"project\(CODEC2\s+VERSION\s+(\d+)\.(\d+)\.(\d+)", cm)
    major, minor, patch = ver.groups()
    tpl = open(os.path.join(up, "cmake", "version.h.in"),
               encoding="utf-8").read()
    tpl = (tpl.replace("@CODEC2_VERSION_MAJOR@", major)
              .replace("@CODEC2_VERSION_MINOR@", minor)
              .replace("#cmakedefine CODEC2_VERSION_PATCH "
                       "@CODEC2_VERSION_PATCH@",
                       "#define CODEC2_VERSION_PATCH " + patch)
              .replace("@CODEC2_VERSION@", "%s.%s.%s" % (major, minor, patch)))
    os.makedirs(os.path.join(OUT, "include", "codec2"))
    with open(os.path.join(OUT, "include", "codec2", "version.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(tpl)

    with open(os.path.join(OUT, "README.md"), "w", newline="\n") as f:
        f.write(
            "# Codec 2 (vendored subset)\n\n"
            "The Codec 2 speech codec by David Rowe and contributors, from "
            "https://github.com/drowe67/codec2 at %s (commit `%s`). "
            "LGPL-2.1: see [`COPYING`](COPYING).\n\n"
            "Only the codec itself is here: the sources in "
            "`scripts/vendor_codec2.py` (`SOURCES`) and the headers they "
            "include, unmodified, plus the codebook tables generated from "
            "upstream's `src/codebook/*.txt` by that script's Python port of "
            "`generate_codebook.c`, and `include/codec2/version.h` "
            "configured from upstream's `cmake/version.h.in`. Regenerate "
            "with:\n\n"
            "```sh\ngit clone --branch 1.2.0 https://github.com/drowe67/"
            "codec2.git /tmp/codec2\npython scripts/vendor_codec2.py "
            "/tmp/codec2\n```\n\n"
            "Linked only into builds with `CONFIG_SOAK_C2_ENCODE` (test "
            "firmware); see `docs/c2_encode_test.md`.\n" % (tag or "1.2.0",
                                                            commit))
    n = len(os.listdir(os.path.join(OUT, "src")))
    print("vendored %d files from %s (%s) into %s" % (n, tag, commit, OUT))


if __name__ == "__main__":
    main()
