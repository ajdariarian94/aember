"""
Run the C++ unit tests (built with `aember qemu build --with-tests`, x64
Debug) in the dev container.

The test binaries are linked against the Poky SDK's glibc, so they are
started through the SDK sysroot's dynamic loader with the SDK and LXC
libraries on the library path; no QEMU needed.
"""

import logging
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from aember.config.libs import ARCH_CONFIG
from aember.config.resources import lxc_dir, sdk_dir
from aember.config.sdk import SDK_SYSROOT_MAP
from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.test")

TEST_ARCH = "x64-poky-linux"
TEST_BUILD_TYPE = "Debug"
_PASSED = re.compile(r"^\[  PASSED  \] (\d+) test", re.MULTILINE)
_FAILED = re.compile(r"^\[  FAILED  \] (\S+\.\S+)", re.MULTILINE)


@dataclass
class SuiteResult:
    name: str
    passed: int
    failed: list[str]
    returncode: int

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.failed and self.passed > 0


class RunTests(Step):
    def __init__(
        self, filter: str | None = None, repeat: int = 1, timeout: int = 300
    ) -> None:
        self.filter = filter
        self.repeat = repeat
        self.timeout = timeout

    def _command(self, root: Path, binary: Path) -> list[str]:
        sysroot = sdk_dir(root, TEST_ARCH) / "sysroots" / SDK_SYSROOT_MAP[TEST_ARCH]
        loader = sysroot / "lib" / ARCH_CONFIG[TEST_ARCH]["loader_name"]
        lib_path = ":".join(
            str(p)
            for p in (
                sysroot / "lib",
                sysroot / "usr" / "lib",
                lxc_dir(root, TEST_ARCH) / "libs",
            )
        )
        if not loader.exists():
            raise FileNotFoundError(
                f"SDK loader {loader} not found; run `aember qemu build --with-tests`."
            )
        return [str(loader), "--library-path", lib_path, str(binary)]

    def _run_one(self, root: Path, binary: Path) -> SuiteResult:
        try:
            res = subprocess.run(
                self._command(root, binary),
                capture_output=True,
                text=True,
                timeout=self.timeout,
            )
            out, rc = res.stdout + res.stderr, res.returncode
        except subprocess.TimeoutExpired:
            out, rc = "", -1
        passed = sum(int(n) for n in _PASSED.findall(out))
        failed = sorted(set(_FAILED.findall(out)))
        result = SuiteResult(binary.name, passed, failed, rc)
        if not result.ok:
            log.error("%s failed (exit %s):\n%s", binary.name, rc, out[-3000:])
        return result

    def run(self, ctx: Context) -> None:
        tests_dir = (
            ctx.root
            / "build"
            / "install"
            / TEST_ARCH
            / TEST_BUILD_TYPE
            / "bin"
            / "tests"
        )
        binaries = sorted(p for p in tests_dir.glob("*-unittest") if p.is_file())
        if self.filter:
            binaries = [b for b in binaries if self.filter in b.name]
        if not binaries:
            raise FileNotFoundError(
                f"No test binaries in {tests_dir}; "
                f"run `aember qemu build --with-tests`."
            )

        failures = 0
        for round_ in range(1, self.repeat + 1):
            total = 0
            for binary in binaries:
                r = self._run_one(ctx.root, binary)
                total += r.passed
                status = (
                    "ok"
                    if r.ok
                    else "FAILED " + ", ".join(r.failed or ["(crash/timeout)"])
                )
                log.info("%-34s %4d passed  %s", r.name, r.passed, status)
                failures += not r.ok
            log.info(
                "Round %d/%d: %d tests in %d suites",
                round_,
                self.repeat,
                total,
                len(binaries),
            )

        if failures:
            raise RuntimeError(f"{failures} test suite run(s) failed")
        log.info("All tests passed")
