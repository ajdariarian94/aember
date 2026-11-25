#!/usr/bin/env python3
"""The mutation workflow's summary: a table of every unit's score from the
build/mutation/<unit>.json reports in DIR, then each unit's survivors
(<unit>.md). Standard library only."""

import json
import sys
from pathlib import Path


def summary(directory: Path) -> str:
    reports = sorted(directory.glob("*.json"), key=lambda p: p.stem)
    lines = [
        "## Mutation scores",
        "",
        "| Unit | Score | Mutants | Survived |",
        "|---|---:|---:|---:|",
    ]
    killed = total = 0
    for path in reports:
        r = json.loads(path.read_text())
        survived = len(r["survived"])
        killed += r["mutants"] - survived
        total += r["mutants"]
        lines.append(f"| {r['unit']} | {r['score']}% | {r['mutants']} | {survived} |")
    if total:
        overall = round(100 * killed / total, 1)
        lines.append(f"| **all** | **{overall}%** | {total} | {total - killed} |")
    lines.append("")
    for path in reports:
        md = path.with_suffix(".md")
        if md.exists():
            lines.append(md.read_text())
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.stdout.write(summary(Path(sys.argv[1])))
