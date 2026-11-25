"""
`aember fuzz`: libFuzzer + ASan + UBSan over the code that reads input
(fuzz/, see fuzz/CMakeLists.txt): services.json, containers.json,
modules.json, network.json, the daemon channel (fd 3), containerd's control
socket, DHCP replies and PID 1's status file.

- Build: clang 18 compiles aember's sources against the x64 SDK sysroot and
  the x64 Debug build's vcpkg packages (configured first if missing) into
  build/fuzz/build/.
- Run: each target for --time seconds (several at once), starting from its
  seeds (fuzz/corpus/<target>/) and the corpus earlier runs grew
  (build/fuzz/corpus/<target>/, kept by CI's cache). A crash, a hang,
  undefined behaviour or a broken property saves the input in
  build/fuzz/crashes/<target>/ and fails the run. Reproduce one with
  `build/fuzz/build/<target> <file>`.
"""

import logging
import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from aember.config.build import build_jobs
from aember.config.resources import sdk_dir
from aember.config.sdk import SDK_SYSROOT_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.cmake.sdk_build import build_dir
from aember.utils.runner import CommandRunner

log = logging.getLogger("aember.fuzz")

ARCH = "x64-poky-linux"
CXX_ENV = "AEMBER_FUZZ_CXX"  # default clang++-18 (the dev image's)
DICTS = {"fuzz_dhcp": "dhcp.dict"}  # the rest: json.dict


def targets(root: Path) -> list[str]:
    """Every fuzz target: fuzz/targets/fuzz_*.cpp."""
    return sorted(p.stem for p in (root / "fuzz" / "targets").glob("fuzz_*.cpp"))


def select(all_targets: list[str], filters: list[str] | None) -> list[str]:
    if not filters:
        return all_targets
    chosen = [t for t in all_targets if any(f in t for f in filters)]
    if not chosen:
        raise ValueError(f"no fuzz target matches {filters}: {all_targets}")
    return chosen


def sysroot(root: Path) -> Path:
    return sdk_dir(root, ARCH) / "sysroots" / SDK_SYSROOT_MAP[ARCH]


def vcpkg_installed(ctx: Context) -> Path:
    return build_dir(ctx) / "vcpkg_installed" / ARCH


def configure_command(root: Path, ctx: Context) -> list[str]:
    return [
        "cmake",
        "-S",
        str(root / "fuzz"),
        "-B",
        str(root / "build" / "fuzz" / "build"),
        f"-DCMAKE_CXX_COMPILER={os.environ.get(CXX_ENV, 'clang++-18')}",
        f"-DAEMBER_SYSROOT={sysroot(root)}",
        f"-DAEMBER_VCPKG_INSTALLED={vcpkg_installed(ctx)}",
    ]


def run_command(root: Path, target: str, seconds: int) -> list[str]:
    fuzz = root / "build" / "fuzz"
    return [
        str(fuzz / "build" / target),
        f"-max_total_time={seconds}",
        "-rss_limit_mb=2048",
        "-timeout=10",  # one input taking longer is a hang
        "-print_final_stats=1",
        f"-dict={root / 'fuzz' / 'dict' / DICTS.get(target, 'json.dict')}",
        f"-artifact_prefix={fuzz / 'crashes' / target}/",
        str(fuzz / "corpus" / target),  # grows; new inputs go here
        str(root / "fuzz" / "corpus" / target),  # the seeds
    ]


@dataclass
class Result:
    target: str
    ok: bool
    runs: int
    coverage: int
    crashes: list[Path]


_RUNS = re.compile(r"stat::number_of_executed_units:\s*(\d+)")
_COV = re.compile(r"\bcov: (\d+)")


def parse_result(target: str, output: str, ok: bool, crashes: list[Path]) -> Result:
    runs = _RUNS.findall(output)
    cov = _COV.findall(output)
    return Result(
        target=target,
        ok=ok and not crashes,
        runs=int(runs[-1]) if runs else 0,
        coverage=int(cov[-1]) if cov else 0,
        crashes=crashes,
    )


def summary(results: list[Result], seconds: int) -> str:
    lines = [
        f"### Fuzzing ({seconds} s per target)",
        "",
        "| Target | Result | Inputs run | Coverage |",
        "|---|---|---|---|",
    ]
    for r in results:
        verdict = "ok" if r.ok else f"**FAILED** ({len(r.crashes)} input(s))"
        lines.append(f"| {r.target} | {verdict} | {r.runs:,} | {r.coverage} |")
    return "\n".join(lines) + "\n"


class FuzzStep(Step):
    def __init__(
        self, filters: list[str] | None, seconds: int, parallel: int | None
    ) -> None:
        self.filters = filters
        self.seconds = seconds
        self.parallel = parallel

    def _build(self, ctx: Context) -> None:
        root = ctx.root
        if not vcpkg_installed(ctx).is_dir():
            log.info("No x64 Debug vcpkg packages yet: configuring the Debug build")
            ConfigureStep().run(ctx)
        CommandRunner.run(configure_command(root, ctx))
        CommandRunner.run(
            [
                "cmake",
                "--build",
                str(root / "build" / "fuzz" / "build"),
                "--parallel",
                str(build_jobs()),
            ]
        )

    def _fuzz(self, root: Path, target: str) -> Result:
        fuzz = root / "build" / "fuzz"
        crashes_dir = fuzz / "crashes" / target
        (fuzz / "corpus" / target).mkdir(parents=True, exist_ok=True)
        crashes_dir.mkdir(parents=True, exist_ok=True)
        before = set(crashes_dir.iterdir())
        log.info("Fuzzing %s for %d s", target, self.seconds)
        res = subprocess.run(
            run_command(root, target, self.seconds),
            capture_output=True,
            text=True,
            errors="replace",
        )
        (fuzz / f"{target}.log").write_text(res.stdout + res.stderr)
        crashes = sorted(set(crashes_dir.iterdir()) - before)
        result = parse_result(target, res.stderr, res.returncode == 0, crashes)
        if not result.ok:
            log.error(
                "%s failed: %s (log: build/fuzz/%s.log)",
                target,
                ", ".join(str(c) for c in crashes) or f"exit {res.returncode}",
                target,
            )
        return result

    def run(self, ctx: Context) -> None:
        chosen = select(targets(ctx.root), self.filters)
        self._build(ctx)
        workers = self.parallel or max(1, min(4, build_jobs()))
        with ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(lambda t: self._fuzz(ctx.root, t), chosen))
        text = summary(results, self.seconds)
        print(text)
        if path := os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(path, "a") as f:
                f.write(text)
        failed = [r.target for r in results if not r.ok]
        if failed:
            raise RuntimeError(
                f"fuzzing failed: {', '.join(failed)} (inputs in build/fuzz/crashes/)"
            )
