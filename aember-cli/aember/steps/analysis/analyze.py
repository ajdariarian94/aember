"""
`aember analyze`: static analysis of aember's own code (libraries/,
services/, executables/; not the tests, vcpkg packages or the SDK).

- clang-tidy (`.clang-tidy`), over the compile database of the x64 Debug
  build. clang 18 parses the code with the SDK's libstdc++ 15 headers
  (`--target`, `-isystem`, `__cpp_concepts` raised so <expected> is
  there), the code itself stays GCC-only. A file clang can't compile shows
  up as clang-diagnostic-error: fix the invocation, never baseline those.
- cppcheck over the same compile database.

Each tool's findings are counted per (file, check, message), without line
numbers, so unrelated edits don't shift them, and compared with
tools/analysis/<tool>-baseline.txt: findings beyond the baseline fail the
run. Fixing one: remove it from the baseline (`--update-baseline` rewrites
the files from the current findings; commit them with the fix). The full
output is in build/analysis/<tool>.txt.
"""

import json
import logging
import re
import subprocess
from collections import Counter
from pathlib import Path

from aember.config.build import build_jobs
from aember.config.resources import sdk_dir
from aember.config.sdk import SDK_SYSROOT_MAP
from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.analyze")

TOOLS = ("clang-tidy", "cppcheck")
OWN = ("libraries/", "services/", "executables/")
Finding = tuple[str, str, str]  # (file relative to the repo, check, message)

_DIAG = re.compile(
    r"^(?P<file>/[^:\s]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<sev>[a-z]+): (?P<msg>.*) \[(?P<check>[^\]]+)\]$"
)


def own_source(path: str, root: Path) -> str | None:
    """Path relative to the repo if it is aember's own non-test code."""
    try:
        rel = Path(path).resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None
    if not rel.startswith(OWN) or "/tests/" in rel:
        return None
    return rel


def parse(output: str, root: Path) -> Counter:
    found: Counter = Counter()
    for line in output.splitlines():
        m = _DIAG.match(line.strip())
        if not m or m["sev"] == "note":
            continue
        rel = own_source(m["file"], root)
        if rel:
            found[(rel, m["check"], m["msg"])] += 1
    return found


def load_baseline(path: Path) -> Counter:
    counts: Counter = Counter()
    if not path.exists():
        return counts
    for line in path.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        count, file, check, msg = line.split("\t", 3)
        counts[(file, check, msg)] = int(count)
    return counts


def write_baseline(path: Path, found: Counter, tool: str) -> None:
    lines = [
        f"# {tool} findings accepted for now (`aember analyze --update-baseline`).",
        "# count<TAB>file<TAB>check<TAB>message. Fix them; don't add to it by hand.",
    ]
    lines += [f"{n}\t{f}\t{c}\t{m}" for (f, c, m), n in sorted(found.items()) if n > 0]
    path.write_text("\n".join(lines) + "\n")


def compare(found: Counter, baseline: Counter) -> tuple[Counter, Counter]:
    """(new: beyond the baseline, fixed: baseline entries no longer found)."""
    return found - baseline, baseline - found


def compile_db(root: Path) -> Path:
    return root / "build" / "configure" / "x64-poky-linux" / "Debug"


def own_files(db_dir: Path, root: Path) -> list[str]:
    entries = json.loads((db_dir / "compile_commands.json").read_text())
    files = {own_source(e["file"], root) for e in entries}
    return sorted(str(root / f) for f in files if f)


def clang_tidy_command(root: Path, db_dir: Path, files: list[str]) -> list[str]:
    sysroot = sdk_dir(root, "x64-poky-linux") / "sysroots"
    cxx = sysroot / SDK_SYSROOT_MAP["x64-poky-linux"] / "usr" / "include" / "c++"
    version = sorted(p.name for p in cxx.iterdir())[-1] if cxx.exists() else "15.2.0"
    return [
        "run-clang-tidy",
        "-p",
        str(db_dir),
        "-j",
        str(build_jobs()),
        "-quiet",
        "-extra-arg=--target=x86_64-linux-gnu",
        f"-extra-arg=-isystem{cxx / version}",
        f"-extra-arg=-isystem{cxx / version / 'x86_64-poky-linux'}",
        # libstdc++ 15 enables <expected> only for concepts >= 202002L,
        # which clang 18 doesn't claim yet (its support is complete enough).
        "-extra-arg=-D__cpp_concepts=202002L",
        "-extra-arg=-Wno-builtin-macro-redefined",
        # GCC-only warnings from AemberWarnings.cmake
        "-extra-arg=-Wno-unknown-warning-option",
        *files,
    ]


def cppcheck_command(root: Path, db_dir: Path) -> list[str]:
    return [
        "cppcheck",
        f"--project={db_dir / 'compile_commands.json'}",
        "--enable=warning,performance,portability,style",
        "--inline-suppr",
        "--std=c++20",
        "--suppress=missingIncludeSystem",
        "--suppress=missingInclude",
        "--suppress=unmatchedSuppression",
        "--suppress=checkersReport",
        f"--suppress=*:{root}/build/*",
        f"-j{build_jobs()}",
        "--quiet",
        "--template={file}:{line}:{column}: {severity}: {message} [{id}]",
    ]


class AnalyzeStep(Step):
    def __init__(
        self, tools: tuple[str, ...] = TOOLS, update_baseline: bool = False
    ) -> None:
        self.tools = tools
        self.update_baseline = update_baseline

    def _run_tool(self, tool: str, root: Path, db_dir: Path) -> str:
        if tool == "clang-tidy":
            cmd = clang_tidy_command(root, db_dir, own_files(db_dir, root))
        else:
            cmd = cppcheck_command(root, db_dir)
        log.info("Running %s", tool)
        res = subprocess.run(cmd, capture_output=True, text=True, cwd=root)
        return res.stdout + res.stderr

    def run(self, ctx: Context) -> None:
        root = ctx.root
        db_dir = compile_db(root)
        if not (db_dir / "compile_commands.json").exists():
            raise FileNotFoundError(f"No compile database in {db_dir}")
        out_dir = root / "build" / "analysis"
        out_dir.mkdir(parents=True, exist_ok=True)
        baseline_dir = root / "tools" / "analysis"

        failed = []
        for tool in self.tools:
            output = self._run_tool(tool, root, db_dir)
            (out_dir / f"{tool}.txt").write_text(output)
            found = parse(output, root)
            baseline_file = baseline_dir / f"{tool}-baseline.txt"
            if self.update_baseline:
                baseline_dir.mkdir(parents=True, exist_ok=True)
                write_baseline(baseline_file, found, tool)
                log.info(
                    "%s: %d findings written to %s",
                    tool,
                    sum(found.values()),
                    baseline_file,
                )
                continue
            new, fixed = compare(found, load_baseline(baseline_file))
            log.info(
                "%s: %d findings, %d new, %d fixed (baseline: %s)",
                tool,
                sum(found.values()),
                sum(new.values()),
                sum(fixed.values()),
                baseline_file.name,
            )
            for (f, c, m), n in sorted(new.items()):
                log.error(
                    "new %s finding: %s: %s [%s]%s",
                    tool,
                    f,
                    m,
                    c,
                    f" (x{n})" if n > 1 else "",
                )
            for (f, c, m), _ in sorted(fixed.items()):
                log.info("fixed (remove from the baseline): %s: %s [%s]", f, m, c)
            if new:
                failed.append(tool)
        if failed:
            raise RuntimeError(f"New static analysis findings: {', '.join(failed)}")
