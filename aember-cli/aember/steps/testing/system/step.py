"""`aember systemtest`: run the system-test scenarios against the packed
image of one architecture."""

from __future__ import annotations

import logging
import os
import time

from aember.core.context import Context
from aember.core.step import Step
from aember.steps.launching.launch import QEMU_PROFILES
from aember.steps.testing.system.scenarios import SCENARIOS, System

log = logging.getLogger("aember.systemtest")


def select(names: list[str] | None) -> list[str]:
    """The scenarios to run, in registration order (all by default)."""
    if not names:
        return list(SCENARIOS)
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        raise ValueError(
            f"unknown scenario(s) {unknown}; known: {', '.join(SCENARIOS)}"
        )
    return [n for n in SCENARIOS if n in names]


def summary_markdown(results: list[tuple[str, bool, float, str]]) -> str:
    lines = [
        "### System tests (QEMU)",
        "",
        "| Scenario | Result | Time |",
        "|---|---|---|",
    ]
    for name, ok, secs, _ in results:
        lines.append(f"| {name} | {'pass' if ok else '**FAIL**'} | {secs:.0f} s |")
    return "\n".join(lines) + "\n"


class SystemTestStep(Step):
    def __init__(
        self,
        names: list[str] | None = None,
        timeout_scale: float = 1.0,
        kvm: bool | None = None,
    ) -> None:
        self.names = names
        self.timeout_scale = timeout_scale
        self.kvm = kvm

    def run(self, ctx: Context) -> None:
        names = select(self.names)
        profile = QEMU_PROFILES.get(ctx.architecture)
        if not profile:
            raise ValueError(f"no QEMU profile for {ctx.architecture}")
        package_dir = ctx.root / "build" / "package" / ctx.architecture / ctx.build_type
        for f in ("kernel", "aember-initramfs.cpio.gz", "aember-rootfs.img"):
            if not (package_dir / f).exists():
                raise FileNotFoundError(
                    f"{package_dir / f} is missing: run `aember app build` and "
                    f"`aember app pack` (-a {ctx.architecture} "
                    f"--build-type {ctx.build_type}) first"
                )
        log_dir = ctx.root / "build" / "system-tests" / ctx.architecture
        log_dir.mkdir(parents=True, exist_ok=True)

        results: list[tuple[str, bool, float, str]] = []
        for name in names:
            system = System(
                profile, package_dir, log_dir, self.timeout_scale, self.kvm, name
            )
            log.info("[%s] %s", name, SCENARIOS[name].description)
            began = time.monotonic()
            try:
                SCENARIOS[name].run(system)
                ok, error = True, ""
            except AssertionError as e:
                ok, error = False, str(e)
            secs = time.monotonic() - began
            results.append((name, ok, secs, error))
            if ok:
                log.info("[%s] pass (%.0f s)", name, secs)
            else:
                log.error("[%s] FAIL (%.0f s): %s", name, secs, error)
                log.error("[%s] console: %s", name, log_dir / f"{name}.log")

        if path := os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(path, "a") as f:
                f.write(summary_markdown(results))
        failed = [n for n, ok, _, _ in results if not ok]
        log.info("%d/%d scenarios passed", len(results) - len(failed), len(results))
        if failed:
            # The failures are logged above; no traceback over them.
            raise SystemExit(f"system tests failed: {', '.join(failed)}")
