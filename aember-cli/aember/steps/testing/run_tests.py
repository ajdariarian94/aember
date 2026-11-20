"""
Run the C++ unit tests (built with `aember qemu build --with-tests`, x64
Debug, or an instrumented variant: `aember test --coverage/--sanitize`) in
the dev container.

The test binaries are linked against the Poky SDK's glibc, so they are
started through the SDK sysroot's dynamic loader with the SDK and LXC
libraries on the library path; no QEMU needed.

- Sanitizer builds run with SANITIZER_ENV: the first error fails the suite.
- `valgrind=True` runs every suite under memcheck (errors and definite
  leaks fail it; suppressions in tools/valgrind/aember.supp).
- `as_root=True` runs them through `sudo -n` with the same environment, so
  the root-only tests (mounts, loop devices, veth) run too; they enter
  private namespaces themselves.
"""

import logging
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from aember.config.libs import ARCH_CONFIG
from aember.config.resources import busybox_dir, lxc_dir, sdk_dir
from aember.config.sdk import SDK_SYSROOT_MAP
from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.test")

TEST_ARCH = "x64-poky-linux"
TEST_BUILD_TYPE = "Debug"
_PASSED = re.compile(r"^\[  PASSED  \] (\d+) test", re.MULTILINE)
_FAILED = re.compile(r"^\[  FAILED  \] (\S+\.\S+)", re.MULTILINE)
_SKIPPED = re.compile(r"^\[  SKIPPED \] (\d+) test", re.MULTILINE)

# halt_on_error: the first report fails the suite (and names the test).
SANITIZER_ENV = {
    "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=0:"
    "detect_stack_use_after_return=1:strict_init_order=1:"
    "check_initialization_order=1",
    "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1",
    "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1",
}
# Memory errors fail any process (exit 99), forked test children included.
# Leaks don't set the exit code: a child that ends with _exit() leaves its
# heap behind, which valgrind calls "definitely lost". Only the test
# process's own leaks count (valgrind_leaked() reads its leak summary).
VALGRIND = [
    "valgrind",
    "--error-exitcode=99",
    "--leak-check=full",
    "--errors-for-leak-kinds=none",
    "--show-leak-kinds=definite",
    "--track-origins=yes",
]
_VG_LINE = re.compile(r"^==(\d+)== ", re.MULTILINE)
_VG_DEFINITELY = re.compile(
    r"^==(\d+)==\s+definitely lost: ([\d,]+) bytes", re.MULTILINE
)


def valgrind_leaked(output: str) -> int:
    """Bytes definitely lost by the test process itself (the first pid
    valgrind prints), not by its forked children."""
    first = _VG_LINE.search(output)
    if not first:
        return 0
    main_pid = first.group(1)
    return sum(
        int(m.group(2).replace(",", ""))
        for m in _VG_DEFINITELY.finditer(output)
        if m.group(1) == main_pid
    )


def test_env(root: Path) -> dict[str, str]:
    """What the tests may use besides the system: the static BusyBox from
    aember-resources (root-manager's pivot test execs it in a new root)."""
    return {
        "AEMBER_TEST_BUSYBOX": str(busybox_dir(root, TEST_ARCH) / "busybox"),
        # The dynamic loader's own allocations (tools/sanitizers/lsan.supp)
        "LSAN_OPTIONS": "suppressions="
        + str(root / "tools" / "sanitizers" / "lsan.supp")
        + ":print_suppressions=0",
    }


def tests_dir(root: Path, variant: str = TEST_BUILD_TYPE) -> Path:
    return root / "build" / "install" / TEST_ARCH / variant / "bin" / "tests"


@dataclass
class SuiteResult:
    name: str
    passed: int
    failed: list[str]
    returncode: int
    skipped: int = 0

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.failed and self.passed > 0


class RunTests(Step):
    def __init__(
        self,
        filter: str | None = None,
        repeat: int = 1,
        timeout: int = 300,
        valgrind: bool = False,
        as_root: bool = False,
        allow_empty: bool = False,
    ) -> None:
        self.filter = filter
        self.repeat = repeat
        self.timeout = timeout
        self.valgrind = valgrind
        self.as_root = as_root
        self.allow_empty = allow_empty
        self.no_aslr = False  # set from ctx.instrument in run()

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
        cmd = [str(loader), "--library-path", lib_path, str(binary)]
        if self.no_aslr:
            # ThreadSanitizer needs ASLR off on kernels with many mmap random
            # bits (WSL 6.18) and re-execs /proc/self/exe to turn it off; that
            # is ld.so here, so the re-exec fails ("execve failed, errno 2").
            cmd = ["setarch", "x86_64", "-R"] + cmd
        if self.valgrind:
            supp = root / "tools" / "valgrind" / "aember.supp"
            cmd = VALGRIND + [f"--suppressions={supp}"] + cmd
        if self.as_root:
            # sudo resets the environment: pass the sanitizer options on.
            env = {**SANITIZER_ENV, **test_env(root)}
            cmd = ["sudo", "-n", "env"] + [f"{k}={v}" for k, v in env.items()] + cmd
        return cmd

    def _env(self, root: Path) -> dict[str, str]:
        return {**os.environ, **SANITIZER_ENV, **test_env(root)}

    def _run_one(self, root: Path, binary: Path) -> SuiteResult:
        try:
            res = subprocess.run(
                self._command(root, binary),
                capture_output=True,
                text=True,
                errors="replace",  # test output can be any bytes
                timeout=self.timeout * (10 if self.valgrind else 1),
                env=self._env(root),
            )
            out, rc = res.stdout + res.stderr, res.returncode
        except subprocess.TimeoutExpired:
            out, rc = "", -1
        if self.valgrind and rc == 0 and (leaked := valgrind_leaked(out)):
            log.error("%s: valgrind: %d bytes definitely lost", binary.name, leaked)
            rc = 98
        passed = sum(int(n) for n in _PASSED.findall(out))
        skipped = sum(int(n) for n in _SKIPPED.findall(out))
        failed = sorted(set(_FAILED.findall(out)))
        result = SuiteResult(binary.name, passed, failed, rc, skipped)
        if not result.ok:
            # The whole output, for diagnosis (and CI artifacts); the log
            # line only has the end of it.
            logs = root / "build" / "test-logs"
            logs.mkdir(parents=True, exist_ok=True)
            (logs / f"{binary.name}.log").write_text(out)
            log.error(
                "%s failed (exit %s), full output in %s:\n%s",
                binary.name,
                rc,
                logs / f"{binary.name}.log",
                out[-3000:],
            )
        return result

    def run(self, ctx: Context) -> None:
        variant = ctx.variant if ctx.build_type else TEST_BUILD_TYPE
        self.no_aslr = ctx.instrument == "tsan"
        directory = tests_dir(ctx.root, variant)
        binaries = sorted(p for p in directory.glob("*-unittest") if p.is_file())
        if self.filter:
            binaries = [b for b in binaries if self.filter in b.name]
        if not binaries and self.allow_empty:
            log.info("No C++ suite matches %r", self.filter)
            return
        if not binaries:
            raise FileNotFoundError(
                f"No test binaries in {directory}; "
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
                # Short: rich wraps long lines and the name got lost.
                log.info(
                    "%s: %d passed, %d skipped, %s",
                    r.name.removesuffix("-unittest"),
                    r.passed,
                    r.skipped,
                    status,
                )
                failures += not r.ok
            log.info(
                "Round %d/%d: %d tests in %d suites",
                round_,
                self.repeat,
                total,
                len(binaries),
            )

        if self.as_root:
            # Root wrote coverage counters (.gcda) into the build tree: give
            # them back, or the next run as dev can't update them.
            build = ctx.root / "build" / "configure" / TEST_ARCH / variant
            if build.exists():
                subprocess.run(
                    ["sudo", "-n", "chown", "-R", f"{os.getuid()}:{os.getgid()}"]
                    + [str(build)],
                    check=False,
                )

        if failures:
            raise RuntimeError(f"{failures} test suite run(s) failed")
        log.info("All tests passed")
