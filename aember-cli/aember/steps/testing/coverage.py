"""
Coverage report of an `aember test --coverage` run.

gcovr reads the .gcda counters the tests wrote next to the objects in
build/configure/x64-poky-linux/Debug-coverage/, through the SDK's gcov (GCC
15's format; the host's gcov can't read it). Output in build/coverage/:

    index.html      line + branch coverage, per file, browsable
    summary.json    gcovr's per-file summary
    components.json per library/service: lines, functions, branches

Only aember's own code counts: libraries/, services/, executables/,
without the tests themselves. Branches the compiler adds for exceptions
and unreachable code are left out, so branch coverage means the code's
own decisions; template instantiations are merged per source line.
"""

import json
import logging
import os
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from aember.config.resources import sdk_dir
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.testing.run_tests import TEST_ARCH

log = logging.getLogger("aember.coverage")

VARIANT = "Debug-coverage"
SOURCES = ("libraries", "services", "executables")


def coverage_dir(root: Path) -> Path:
    return root / "build" / "coverage"


def object_dir(root: Path) -> Path:
    return root / "build" / "configure" / TEST_ARCH / VARIANT


def gcov_tool(root: Path) -> Path:
    return (
        sdk_dir(root, TEST_ARCH)
        / "sysroots"
        / "x86_64-pokysdk-linux"
        / "usr"
        / "bin"
        / "x86_64-poky-linux"
        / "x86_64-poky-linux-gcov"
    )


def component(path: str) -> str | None:
    """The library or service a source file belongs to:
    libraries/utils/ipc/src/hub.cpp → utils/ipc,
    libraries/aember-init/... → aember-init, services/netd/... → netd,
    executables/aember/main.cpp → aember. None for anything else (tests,
    generated files, dependencies)."""
    parts = Path(path).parts
    # tests/, and shared test helpers (services/tests-common/)
    if any(p.startswith("tests") for p in parts) or len(parts) < 3:
        return None
    top, name = parts[0], parts[1]
    if top == "libraries":
        if name == "utils":
            return f"utils/{parts[2]}" if len(parts) > 3 else None
        return name
    if top in ("services", "executables"):
        return name
    return None


@dataclass
class Totals:
    lines: int = 0
    lines_covered: int = 0
    functions: int = 0
    functions_covered: int = 0
    branches: int = 0
    branches_covered: int = 0
    files: list[str] = field(default_factory=list)

    @staticmethod
    def percent(covered: int, total: int) -> float | None:
        return round(100.0 * covered / total, 1) if total else None

    def as_dict(self) -> dict:
        return {
            "lines": self.lines,
            "lines_covered": self.lines_covered,
            "line_percent": self.percent(self.lines_covered, self.lines),
            "functions": self.functions,
            "functions_covered": self.functions_covered,
            "function_percent": self.percent(self.functions_covered, self.functions),
            "branches": self.branches,
            "branches_covered": self.branches_covered,
            "branch_percent": self.percent(self.branches_covered, self.branches),
            "files": sorted(self.files),
        }


def summarize(files: list[dict]) -> dict[str, Totals]:
    """gcovr --json-summary "files" entries → totals per component."""
    result: dict[str, Totals] = {}
    for f in files:
        name = component(f["filename"])
        if name is None:
            continue
        t = result.setdefault(name, Totals())
        t.lines += f.get("line_total", 0)
        t.lines_covered += f.get("line_covered", 0)
        t.functions += f.get("function_total", 0)
        t.functions_covered += f.get("function_covered", 0)
        t.branches += f.get("branch_total", 0)
        t.branches_covered += f.get("branch_covered", 0)
        t.files.append(f["filename"])
    return dict(sorted(result.items()))


def total(components: dict[str, Totals]) -> Totals:
    all_ = Totals()
    for t in components.values():
        all_.lines += t.lines
        all_.lines_covered += t.lines_covered
        all_.functions += t.functions
        all_.functions_covered += t.functions_covered
        all_.branches += t.branches
        all_.branches_covered += t.branches_covered
    return all_


def gcovr_command(root: Path, out: Path) -> list[str]:
    cmd = [
        "gcovr",
        "--root",
        str(root),
        "--gcov-executable",
        str(gcov_tool(root)),
        "--exclude-throw-branches",
        "--exclude-unreachable-branches",
        # A template's lines count once, covered if any instantiation ran
        # (every log_.error() call site instantiates Logger::Log).
        "--merge-lines",
        "--exclude",
        r".*/tests/.*",
        "--json-summary-pretty",
        "--json-summary",
        str(out / "summary.json"),
        "--html-details",
        str(out / "index.html"),
        "--html-title",
        "aember coverage",
    ]
    for source in SOURCES:
        cmd += ["--filter", rf"{source}/"]
    return cmd + [str(object_dir(root))]


def _fmt(p: float | None) -> str:
    return "   -  " if p is None else f"{p:5.1f}%"


class CoverageReport(Step):
    def run(self, ctx: Context) -> None:
        root = ctx.root
        if not shutil.which("gcovr"):
            raise FileNotFoundError("gcovr not found: pip install -r requirements.txt")
        if not gcov_tool(root).exists():
            raise FileNotFoundError(f"{gcov_tool(root)} not found (SDK missing?)")
        out = coverage_dir(root)
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)

        log.info("Collecting coverage (gcovr)")
        subprocess.run(gcovr_command(root, out), check=True, cwd=root)

        files = json.loads((out / "summary.json").read_text())["files"]
        components = summarize(files)
        report = {name: t.as_dict() for name, t in components.items()}
        report["total"] = total(components).as_dict()
        (out / "components.json").write_text(json.dumps(report, indent=2) + "\n")

        log.info(
            "%-22s %8s %8s %8s %7s", "component", "lines", "funcs", "branch", "lines#"
        )
        for name, t in [*components.items(), ("TOTAL", total(components))]:
            d = t.as_dict()
            log.info(
                "%-22s %8s %8s %8s %7d",
                name,
                _fmt(d["line_percent"]),
                _fmt(d["function_percent"]),
                _fmt(d["branch_percent"]),
                t.lines,
            )
        log.info("Coverage report: %s", out / "index.html")
        ctx.data["coverage"] = report
        if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(summary, "a") as f:
                f.write(markdown(components))


def markdown(components: dict[str, Totals]) -> str:
    """The per-component table for a CI job summary."""
    rows = [
        "## Coverage",
        "",
        "| component | lines | functions | branches | lines total |",
        "|---|---:|---:|---:|---:|",
    ]
    for name, t in [*components.items(), ("**total**", total(components))]:
        d = t.as_dict()
        rows.append(
            f"| {name} | {_fmt(d['line_percent']).strip()} "
            f"| {_fmt(d['function_percent']).strip()} "
            f"| {_fmt(d['branch_percent']).strip()} | {t.lines} |"
        )
    return "\n".join(rows) + "\n"


class ResetCoverage(Step):
    """Delete the counters of earlier runs, so the report is this run's."""

    def run(self, ctx: Context) -> None:
        objects = object_dir(ctx.root)
        n = 0
        for gcda in objects.rglob("*.gcda"):
            gcda.unlink()
            n += 1
        log.debug("Removed %d old .gcda files", n)
