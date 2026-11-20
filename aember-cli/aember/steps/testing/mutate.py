"""
`aember mutate -k <suite>`: mutation testing of one library or service.

Mull (the usual tool) needs clang, and aember builds with GCC only, so this
is a small source-level mutator:

1. Each line of the sources under test (not comments, not string or
   character literals, not preprocessor lines) is scanned for operators
   with an obvious opposite (MUTATIONS): == / !=, < / >=, && / ||,
   true / false, + 1 / - 1, a `!` in front of a call, ...
2. One mutant at a time is written into the source file, only the test
   binary is rebuilt (incremental, in the x64 Debug build tree), and the
   suite runs with a time limit.
3. Killed: the build or a test failed (good). Survived: every test still
   passed, so no test checks that line. Timeout counts as killed.
4. The original is always restored (a .mutate-backup copy next to it is
   put back first, so even a killed run can't leave a mutant behind).

The report goes to build/mutation/<suite>.json; the survivors are what to
write tests for. Slow by nature: --max limits the mutants per run.
"""

import json
import logging
import random
import re
import shutil
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path

from aember.config.build import JOBS_ENV, build_jobs
from aember.config.resources import sdk_dir
from aember.config.sdk import SDK_ENV_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.testing.run_tests import TEST_ARCH, RunTests

log = logging.getLogger("aember.mutate")

BACKUP_SUFFIX = ".mutate-backup"

# (pattern, replacement, name). Patterns match whole operators/words only.
MUTATIONS: list[tuple[str, str, str]] = [
    (r"==", "!=", "eq->ne"),
    (r"!=", "==", "ne->eq"),
    (r"(?<![<>=!])<=(?!=)", ">", "le->gt"),
    (r"(?<![<>=!])>=(?!=)", "<", "ge->lt"),
    (r"(?<![<>\-=!])<(?![<=])", ">=", "lt->ge"),
    (r"(?<![<>\-=!])>(?![>=])", "<=", "gt->le"),
    (r"&&", "||", "and->or"),
    (r"\|\|", "&&", "or->and"),
    (r"\btrue\b", "false", "true->false"),
    (r"\bfalse\b", "true", "false->true"),
    (r"\+ 1\b", "- 1", "plus1->minus1"),
    (r"- 1\b", "+ 1", "minus1->plus1"),
    (r"!(?=[A-Za-z_(])", "", "drop-not"),
]

_STRING = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
# Template brackets and stream/arrow operators look like comparisons: skip
# lines where < or > belong to them.
_NOT_COMPARISON = re.compile(r"template\s*<|#include|->|<<|>>|::|std::\w+<|\w+<\w")


@dataclass(frozen=True)
class Mutant:
    file: str  # relative to the repo
    line: int  # 1-based
    column: int  # 0-based, in the original line
    original: str
    replacement: str
    name: str

    def apply(self, text: str) -> str:
        lines = text.splitlines(keepends=True)
        line = lines[self.line - 1]
        end = self.column + len(self.original)
        assert line[self.column : end] == self.original, (self, line)
        lines[self.line - 1] = line[: self.column] + self.replacement + line[end:]
        return "".join(lines)


def _code_spans(line: str) -> list[tuple[int, int]]:
    """Parts of @p line that are code: outside string/char literals and
    before a // comment."""
    spans: list[tuple[int, int]] = []
    pos = 0
    for m in _STRING.finditer(line):
        spans.append((pos, m.start()))
        pos = m.end()
    spans.append((pos, len(line)))
    result = []
    for start, end in spans:
        comment = line.find("//", start, end)
        if comment >= 0:
            result.append((start, comment))
            break
        result.append((start, end))
    return result


def find_mutants(rel_path: str, text: str) -> list[Mutant]:
    mutants: list[Mutant] = []
    in_block_comment = False
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw
        stripped = line.strip()
        if in_block_comment:
            if "*/" in line:
                in_block_comment = False
            continue
        if stripped.startswith("/*"):
            in_block_comment = "*/" not in stripped
            continue
        if not stripped or stripped.startswith(("//", "*", "#")):
            continue
        skip_angles = bool(_NOT_COMPARISON.search(line))
        taken: set[int] = set()
        for pattern, replacement, name in MUTATIONS:
            if skip_angles and name in ("lt->ge", "gt->le", "le->gt", "ge->lt"):
                continue
            for start, end in _code_spans(line):
                for m in re.finditer(pattern, line[start:end]):
                    col = start + m.start()
                    if col in taken:
                        continue
                    taken.add(col)
                    mutants.append(
                        Mutant(rel_path, number, col, m.group(0), replacement, name)
                    )
    return mutants


@dataclass
class Result:
    mutant: Mutant
    status: str  # killed | survived | timeout | build-error


def score(results: list[Result]) -> float | None:
    """Killed (incl. timeouts and mutants that don't compile) / all."""
    if not results:
        return None
    killed = sum(r.status != "survived" for r in results)
    return round(100.0 * killed / len(results), 1)


def restore_backups(paths: list[Path]) -> None:
    """Put back originals a killed earlier run left behind."""
    for path in paths:
        backup = path.with_name(path.name + BACKUP_SUFFIX)
        if backup.exists():
            log.warning("Restoring %s from an interrupted run", path)
            shutil.move(backup, path)


def suite_sources(root: Path, suite: str) -> list[Path]:
    """Sources of the library/service a test suite belongs to: the src/
    directory next to the tests/ that builds <suite>-unittest."""
    for cmake in sorted(root.glob("**/tests/CMakeLists.txt")):
        if "build" in cmake.relative_to(root).parts:
            continue
        if f"{suite}-unittest" in cmake.read_text():
            src = cmake.parent.parent / "src"
            return sorted(src.glob("*.cpp")) if src.exists() else []
    return []


def sub_build_dir(root: Path, rel_source: str) -> Path:
    """The ExternalProject build tree that compiles @p rel_source."""
    top = rel_source.split("/", 1)[0]
    name = {"libraries": "aember-libs"}.get(top, top)
    return (
        root
        / "build"
        / "configure"
        / TEST_ARCH
        / "Debug"
        / f"{name}-prefix"
        / "src"
        / f"{name}-build"
    )


class MutateStep(Step):
    def __init__(
        self,
        suite: str,
        max_mutants: int = 50,
        seed: int = 1,
        timeout: int = 120,
        files: list[str] | None = None,
    ) -> None:
        self.suite = suite
        self.max_mutants = max_mutants
        self.seed = seed
        self.timeout = timeout
        self.files = files

    def _build(self, root: Path, build_dir: Path, target: str) -> bool:
        env_script = sdk_dir(root, TEST_ARCH) / SDK_ENV_MAP[TEST_ARCH]
        cmd = (
            f"source {env_script} >/dev/null && unset CC CXX && "
            f"export {JOBS_ENV}={build_jobs()} && "
            f"cmake --build {build_dir} --target {target} --parallel {build_jobs()}"
        )
        res = subprocess.run(
            ["bash", "-c", cmd], capture_output=True, text=True, cwd=root
        )
        return res.returncode == 0

    def _run_suite(self, root: Path, binary: Path) -> str:
        runner = RunTests(timeout=self.timeout)
        try:
            res = subprocess.run(
                runner._command(root, binary),
                capture_output=True,
                text=True,
                errors="replace",  # test output can be any bytes
                timeout=self.timeout,
                env=runner._env(root),
            )
        except subprocess.TimeoutExpired:
            return "timeout"
        return "survived" if res.returncode == 0 else "killed"

    def run(self, ctx: Context) -> None:
        root = ctx.root
        sources = suite_sources(root, self.suite)
        if self.files:
            sources = [s for s in sources if any(f in str(s) for f in self.files)]
        if not sources:
            raise FileNotFoundError(f"No sources for test suite '{self.suite}'")
        restore_backups(sources)

        mutants = [
            m
            for src in sources
            for m in find_mutants(src.relative_to(root).as_posix(), src.read_text())
        ]
        random.Random(self.seed).shuffle(mutants)
        mutants = mutants[: self.max_mutants]
        target = f"{self.suite}-unittest"
        binary_build = sub_build_dir(root, mutants[0].file) if mutants else None
        log.info("%d mutants of %d file(s) for %s", len(mutants), len(sources), target)

        # The unmutated suite must build and pass, or nothing can be judged.
        if not binary_build or not self._build(root, binary_build, target):
            raise RuntimeError(f"{target} doesn't build; build Debug with tests")
        binary = self._binary(binary_build, target)
        if self._run_suite(root, binary) != "survived":
            raise RuntimeError(f"{target} fails without mutations; fix it first")

        results: list[Result] = []
        for i, mutant in enumerate(mutants, start=1):
            path = root / mutant.file
            backup = path.with_name(path.name + BACKUP_SUFFIX)
            original = path.read_text()
            shutil.copy2(path, backup)
            try:
                path.write_text(mutant.apply(original))
                if not self._build(root, binary_build, target):
                    status = "build-error"
                else:
                    status = self._run_suite(root, binary)
            finally:
                shutil.move(backup, path)
            results.append(Result(mutant, status))
            log.info(
                "[%d/%d] %-11s %s:%d %s",
                i,
                len(mutants),
                status,
                mutant.file,
                mutant.line,
                mutant.name,
            )
        # Leave the build tree matching the real sources again.
        self._build(root, binary_build, target)

        out = root / "build" / "mutation"
        out.mkdir(parents=True, exist_ok=True)
        report = {
            "suite": self.suite,
            "score": score(results),
            "mutants": len(results),
            "survived": [asdict(r.mutant) for r in results if r.status == "survived"],
            "results": [{**asdict(r.mutant), "status": r.status} for r in results],
        }
        (out / f"{self.suite}.json").write_text(json.dumps(report, indent=2) + "\n")
        log.info(
            "Mutation score %s%% (%d mutants, %d survived): %s",
            report["score"],
            len(results),
            len(report["survived"]),
            out / f"{self.suite}.json",
        )

    @staticmethod
    def _binary(build_dir: Path, target: str) -> Path:
        found = sorted(build_dir.rglob(target))
        found = [p for p in found if p.is_file()]
        if not found:
            raise FileNotFoundError(f"{target} not found under {build_dir}")
        return found[0]
