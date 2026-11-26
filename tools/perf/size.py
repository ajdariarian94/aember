#!/usr/bin/env python3
"""
Size breakdown of aember's binaries (doc/performance.md): which library the
bytes of each binary come from, by symbol, with Bloaty McBloatface.

    tools/perf/size.py BINARY... [--top N]

The binaries must keep their symbols: a Release build without stripping
(tools/perf/variant.sh ARCH NAME -Os syms). Bloaty runs in Docker
(tools/perf/bloaty.Dockerfile, pinned v1.1; built on first use as
aember-bloaty:1.1). Sizes are VM sizes: what is loaded, without debug info.
"""

from __future__ import annotations

import argparse
import collections
import csv
import io
import re
import subprocess
import sys
from pathlib import Path

IMAGE = "aember-bloaty:1.1"
HERE = Path(__file__).resolve().parent

# First match wins.
BUCKETS = [
    ("spdlog", r"\bspdlog::"),
    ("fmt", r"\bfmt::"),
    ("nlohmann::json", r"nlohmann::"),
    ("CLI11", r"\bCLI::|_ZN[KS]?3CLI"),
    ("std::format", r"std::__format|std::__unicode|basic_format|vformat|__format"),
    ("sections (no symbol)", r"^\["),
    ("aember", r"\baember\b|aember::|Aember|pid1::"),
    ("libstdc++ templates", r"(^|\W)(std|__gnu_cxx)::"),
]


def bucket(symbol: str) -> str:
    for name, pattern in BUCKETS:
        if re.search(pattern, symbol):
            return name
    return "other"


def ensure_image() -> None:
    have = (
        subprocess.run(
            ["docker", "image", "inspect", IMAGE], capture_output=True
        ).returncode
        == 0
    )
    if not have:
        subprocess.run(
            [
                "docker",
                "build",
                "-t",
                IMAGE,
                "-f",
                str(HERE / "bloaty.Dockerfile"),
                str(HERE),
            ],
            check=True,
        )


def symbols(binary: Path) -> list[tuple[str, int]]:
    binary = binary.resolve()
    out = subprocess.run(
        [
            "docker",
            "run",
            "--rm",
            "-v",
            f"{binary.parent}:/w:ro",
            IMAGE,
            "-d",
            "symbols",
            "-n",
            "0",
            "--csv",
            "--demangle=full",
            f"/w/{binary.name}",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [(r["symbols"], int(r["vmsize"])) for r in csv.DictReader(io.StringIO(out))]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("binaries", nargs="+", type=Path)
    ap.add_argument(
        "--top",
        type=int,
        default=0,
        help="also list the N biggest symbols of each binary",
    )
    args = ap.parse_args()
    ensure_image()
    for b in args.binaries:
        syms = symbols(b)
        total = sum(v for _, v in syms)
        sizes = collections.Counter()
        for s, v in syms:
            sizes[bucket(s)] += v
        print(f"{b}: {total / 1024:.0f} KiB")
        for name, v in sizes.most_common():
            print(f"  {name:22s} {v / 1024:7.1f} KiB {100 * v / total:5.1f} %")
        for s, v in sorted(syms, key=lambda x: -x[1])[: args.top]:
            print(f"    {v / 1024:6.1f} KiB  {s[:110]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
