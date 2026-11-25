#!/usr/bin/env python3
"""The mutation workflow's matrix: every unit `aember mutate` knows (or the
ones named on the command line), as JSON [{"unit": ..., "suite": ...}]
(suite: its first test suite, built and run before mutating). Standard
library only: it runs on the bare runner."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "aember-cli"))
from aember.steps.testing.mutate import test_units  # noqa: E402


def matrix(root: Path, wanted: list[str]) -> list[dict[str, str]]:
    units = test_units(root)
    unknown = set(wanted) - {u.name for u in units}
    if unknown:
        raise SystemExit(f"unknown units: {' '.join(sorted(unknown))}")
    return [
        {"unit": u.name, "suite": u.suites[0]}
        for u in units
        if not wanted or u.name in wanted
    ]


if __name__ == "__main__":
    print(json.dumps(matrix(ROOT, sys.argv[1:])))
