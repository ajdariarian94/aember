"""
`aember mutate -k <unit>`: mutation testing of one library or service.

A unit is a directory with a tests/CMakeLists.txt: its sources (src/*.cpp)
are mutated and every test suite of that tests/ directory is run against
each mutant (containerd's six suites, netd's three, ...). `-k` takes the
unit's directory name or the name of one of its suites.

Mull (the usual tool) needs clang, and aember builds with GCC only, so this
is a small source-level mutator:

1. Each line of the sources under test (not comments, not string or
   character literals, not preprocessor lines) is scanned for operators
   with an obvious opposite (MUTATIONS): == / !=, < / >=, && / ||,
   true / false, + 1 / - 1, a `!` in front of a call, ...
2. One mutant at a time is written into the source file, only the unit's
   test binaries are rebuilt (incremental, in the x64 Debug build tree),
   and its suites run with a time limit (as root with --root).
3. Killed: the build or a test failed (good). Survived: every test still
   passed, so no test checks that line. Timeout counts as killed.
4. The original is always restored (a .mutate-backup copy next to it is
   put back first, so even a killed run can't leave a mutant behind).

The report goes to build/mutation/<unit>.json (+ <unit>.md, a summary for
CI); the survivors are what to write tests for, or to judge equivalent
(same behaviour another way). Slow by nature: --max limits the mutants per
run (a sample, the same for the same --seed); CI runs every unit weekly
(.github/workflows/mutation.yml).
"""

import json
import logging
import os
import random
import re
import shutil
import signal
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


def restore(backup: Path, path: Path) -> None:
    """Put the original back with a new mtime. shutil.move keeps the
    backup's (older) mtime, and make then kept the object compiled from the
    last mutant: a mutant stayed in the normal build."""
    shutil.move(backup, path)
    os.utime(path)


def restore_backups(paths: list[Path]) -> None:
    """Put back originals a killed earlier run left behind."""
    for path in paths:
        backup = path.with_name(path.name + BACKUP_SUFFIX)
        if backup.exists():
            log.warning("Restoring %s from an interrupted run", path)
            restore(backup, path)


@dataclass(frozen=True)
class Unit:
    """A tests/ directory: @p name is its parent directory's name."""

    name: str
    suites: tuple[str, ...]  # <suite>-unittest targets, sorted
    sources: tuple[Path, ...]  # src/*.cpp next to tests/


_SUITE = re.compile(r"([A-Za-z0-9_-]+)-unittest")


def test_units(root: Path) -> list[Unit]:
    """Every library/service with unit tests, sorted by name."""
    units = []
    for cmake in sorted(root.glob("**/tests/CMakeLists.txt")):
        rel = cmake.relative_to(root).parts
        if "build" in rel or rel[0] not in ("libraries", "services"):
            continue
        suites = tuple(sorted(set(_SUITE.findall(cmake.read_text()))))
        src = cmake.parent.parent / "src"
        sources = tuple(sorted(src.glob("*.cpp"))) if src.exists() else ()
        if suites and sources:
            units.append(Unit(cmake.parent.parent.name, suites, sources))
    return sorted(units, key=lambda u: u.name)


def find_unit(root: Path, name: str) -> Unit | None:
    """The unit called @p name, or the one with the suite @p name."""
    units = test_units(root)
    for unit in units:
        if unit.name == name:
            return unit
    for unit in units:
        if name in unit.suites:
            return unit
    return None


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


def report_markdown(unit: Unit, results: list[Result], root: Path) -> str:
    """A short Markdown summary (CI job summary): score and survivors with
    their source line."""
    survived = [r.mutant for r in results if r.status == "survived"]
    lines = [
        f"### {unit.name}: {score(results)}% "
        f"({len(results)} mutants, {len(survived)} survived)",
        "",
    ]
    if survived:
        lines += ["| Where | Mutation | Line |", "|---|---|---|"]
        for m in sorted(survived, key=lambda m: (m.file, m.line, m.column)):
            text = (root / m.file).read_text().splitlines()[m.line - 1].strip()
            code = text.replace("|", "\\|")
            lines.append(f"| `{m.file}:{m.line}` | {m.name} | `{code}` |")
        lines.append("")
    return "\n".join(lines) + "\n"


# A PID namespace for root runs: everything the suite started dies with it.
UNSHARE = ["unshare", "--pid", "--fork", "--kill-child", "--mount", "--mount-proc"]


def isolated(cmd: list[str], as_root: bool) -> list[str]:
    """@p cmd (RunTests' command) in a PID namespace when it runs as root
    (`sudo -n env K=V... <cmd>`: unshare goes after the variables)."""
    if not as_root:
        return cmd
    i = cmd.index("env") + 1
    while i < len(cmd) and "=" in cmd[i] and not cmd[i].startswith("/"):
        i += 1
    return cmd[:i] + UNSHARE + cmd[i:]


def kill_group(proc: subprocess.Popen) -> None:
    """SIGKILL @p proc's process group (its own session) and reap it."""
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:  # sudo dropped to root children: kill sudo
        proc.kill()
    proc.wait()


class MutateStep(Step):
    def __init__(
        self,
        unit: str,
        max_mutants: int = 50,
        seed: int = 1,
        timeout: int = 120,
        files: list[str] | None = None,
        as_root: bool = False,
    ) -> None:
        self.unit = unit
        self.max_mutants = max_mutants  # 0: every mutant
        self.seed = seed
        self.timeout = timeout
        self.files = files
        self.as_root = as_root

    def _build(self, root: Path, build_dir: Path, targets: list[str]) -> bool:
        env_script = sdk_dir(root, TEST_ARCH) / SDK_ENV_MAP[TEST_ARCH]
        cmd = (
            f"source {env_script} >/dev/null && unset CC CXX && "
            f"export {JOBS_ENV}={build_jobs()} && "
            f"cmake --build {build_dir} --target {' '.join(targets)} "
            f"--parallel {build_jobs()}"
        )
        res = subprocess.run(
            ["bash", "-c", cmd], capture_output=True, text=True, cwd=root
        )
        return res.returncode == 0

    def _run_suites(self, root: Path, binaries: list[Path]) -> str:
        """killed as soon as one suite fails (or times out), else survived."""
        for binary in binaries:
            status = self._run_suite(root, binary)
            if status != "survived":
                return status
        return "survived"

    def _run_suite(self, root: Path, binary: Path) -> str:
        """A mutant can turn a process-manager's fork() around, and the test
        then kill()s pid 0 (its own process group) or -1 (everything it
        may signal): this run died with it once. So each suite runs in its
        own session, and as root in its own PID namespace too; on a timeout
        the whole group goes."""
        runner = RunTests(timeout=self.timeout, as_root=self.as_root)
        cmd = isolated(runner._command(root, binary), self.as_root)
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=runner._env(root),
            start_new_session=True,
        )
        try:
            code = proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            kill_group(proc)
            return "timeout"
        return "survived" if code == 0 else "killed"

    def run(self, ctx: Context) -> None:
        root = ctx.root
        unit = find_unit(root, self.unit)
        if unit is None:
            names = ", ".join(u.name for u in test_units(root))
            raise FileNotFoundError(f"No unit or suite '{self.unit}' (units: {names})")
        sources = list(unit.sources)
        if self.files:
            sources = [s for s in sources if any(f in str(s) for f in self.files)]
        if not sources:
            raise FileNotFoundError(f"No sources of '{unit.name}' match {self.files}")
        restore_backups(sources)

        mutants = [
            m
            for src in sources
            for m in find_mutants(src.relative_to(root).as_posix(), src.read_text())
        ]
        random.Random(self.seed).shuffle(mutants)
        if self.max_mutants > 0:
            mutants = mutants[: self.max_mutants]
        targets = [f"{s}-unittest" for s in unit.suites]
        build_dir = sub_build_dir(root, sources[0].relative_to(root).as_posix())
        log.info(
            "%d mutants of %d file(s) for %s (%s)",
            len(mutants),
            len(sources),
            unit.name,
            ", ".join(targets),
        )

        # The unmutated suites must build and pass, or nothing can be judged.
        if not self._build(root, build_dir, targets):
            raise RuntimeError(
                f"{unit.name}'s tests don't build; build Debug with tests"
            )
        binaries = [self._binary(build_dir, t) for t in targets]
        if self._run_suites(root, binaries) != "survived":
            raise RuntimeError(
                f"{unit.name}'s tests fail without mutations; fix them first"
            )

        results: list[Result] = []
        for i, mutant in enumerate(mutants, start=1):
            path = root / mutant.file
            backup = path.with_name(path.name + BACKUP_SUFFIX)
            original = path.read_text()
            shutil.copy2(path, backup)
            try:
                path.write_text(mutant.apply(original))
                if not self._build(root, build_dir, targets):
                    status = "build-error"
                else:
                    status = self._run_suites(root, binaries)
            finally:
                restore(backup, path)
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
        self._build(root, build_dir, targets)

        out = root / "build" / "mutation"
        out.mkdir(parents=True, exist_ok=True)
        report = {
            "unit": unit.name,
            "suites": list(unit.suites),
            "score": score(results),
            "mutants": len(results),
            "seed": self.seed,
            "survived": [asdict(r.mutant) for r in results if r.status == "survived"],
            "results": [{**asdict(r.mutant), "status": r.status} for r in results],
        }
        (out / f"{unit.name}.json").write_text(json.dumps(report, indent=2) + "\n")
        (out / f"{unit.name}.md").write_text(report_markdown(unit, results, root))
        log.info(
            "Mutation score %s%% (%d mutants, %d survived): %s",
            report["score"],
            len(results),
            len(report["survived"]),
            out / f"{unit.name}.json",
        )

    @staticmethod
    def _binary(build_dir: Path, target: str) -> Path:
        found = sorted(build_dir.rglob(target))
        found = [p for p in found if p.is_file()]
        if not found:
            raise FileNotFoundError(f"{target} not found under {build_dir}")
        return found[0]
