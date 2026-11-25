"""
Run the CLI's own Python tests (aember-cli/tests, stdlib unittest; the
property tests also need Hypothesis from requirements.txt and are skipped
without it): `aember test` runs them before the C++ suites.
"""

import logging
import subprocess
import sys

from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.test")


class RunCliTests(Step):
    def run(self, ctx: Context) -> None:
        cli = ctx.root / "aember-cli"
        result = subprocess.run(
            [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-t", "."],
            cwd=cli,
            capture_output=True,
            text=True,
        )
        summary = (result.stderr.strip().splitlines() or ["(no output)"])[-3:]
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            raise RuntimeError("aember-cli tests failed")
        log.info("aember-cli tests: %s", " ".join(s.strip() for s in summary))
