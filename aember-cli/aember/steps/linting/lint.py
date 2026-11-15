import concurrent.futures
import logging
import subprocess
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner

log = logging.getLogger("aember.lint")

EXCLUDE_PATHS = ["build", "vcpkg", ".venv"]
CPP_SCAN_FOLDERS = ["libraries", "executables", "examples"]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _run(cmd: str) -> bool:
    """Run a shell command. Returns True if successful."""
    try:
        CommandRunner.run(cmd, shell=True)
        return True
    except Exception:
        return False


def _find_cpp_files(root: str) -> list[str]:
    """Find C++ files only in CPP_SCAN_FOLDERS."""
    folders = [f"./{f}" for f in CPP_SCAN_FOLDERS if Path(f).is_dir()]
    if not folders:
        return []

    result = subprocess.run(
        [
            "find",
            *folders,
            "(",
            "-name",
            "*.cpp",
            "-o",
            "-name",
            "*.hpp",
            "-o",
            "-name",
            "*.h",
            ")",
            "-not",
            "-path",
            "*/.*",
        ],
        capture_output=True,
        text=True,
    )
    return result.stdout.split()


def _clang_format_file(args: tuple[str, bool]) -> bool:
    f, fix = args
    flags = ["-i"] if fix else ["--dry-run"]
    result = subprocess.run(
        ["clang-format", *flags, "--Werror", "-style=file", f],
        capture_output=True,
    )
    return result.returncode == 0


def _format_cmake_files(root: Path) -> None:
    """Format CMakeLists.txt in root + CPP_SCAN_FOLDERS."""
    files = []
    if (root / "CMakeLists.txt").is_file():
        files.append(root / "CMakeLists.txt")
    for subfolder in CPP_SCAN_FOLDERS:
        folder = root / subfolder
        if folder.is_dir():
            files.extend(folder.rglob("CMakeLists.txt"))
    if files:
        subprocess.run(["cmake-format", "-i", *map(str, files)], check=True)
    else:
        log.debug("No CMakeLists.txt files found to format")


# ---------------------------------------------------------------------------
# Step
# ---------------------------------------------------------------------------


class LintStep(Step):

    def __init__(self, fix: bool = True) -> None:
        self.fix = fix

    def run(self, ctx: Context | None = None) -> None:
        log.info("Starting linting (fix=%s)", self.fix)

        exit_code = 0
        root = "."
        fix = self.fix
        cmake_root = ctx.root if ctx else Path.cwd()

        # ---------------------------------------------------------------------------
        # Python — ruff + black (no isort — black handles import formatting)
        # ---------------------------------------------------------------------------
        log.info("Linting Python")

        ruff_exclude = ",".join(EXCLUDE_PATHS + ["__init__.py"])
        ruff_cmd = f"ruff check {root} --extend-exclude {ruff_exclude}"
        if fix:
            ruff_cmd += " --fix"
        if not _run(ruff_cmd):
            exit_code |= 1
            log.warning("Ruff reported issues")

        black_exclude = "|".join(f"{p}(/.*)?" for p in EXCLUDE_PATHS) + "|__init__.py"
        black_cmd = f"black {root} --exclude '{black_exclude}'"
        if not fix:
            black_cmd += " --check"
        if not _run(black_cmd):
            exit_code |= 1
            log.warning("black reported issues")

        # ---------------------------------------------------------------------------
        # C++ — parallel clang-format over libraries/ executables/ examples/ only
        # ---------------------------------------------------------------------------
        log.info("Linting C++ with clang-format")

        cpp_files = _find_cpp_files(root)
        log.debug("Found %d C++ files to lint", len(cpp_files))

        if cpp_files:
            with concurrent.futures.ThreadPoolExecutor() as ex:
                results = list(
                    ex.map(_clang_format_file, [(f, fix) for f in cpp_files])
                )
            if not all(results):
                exit_code |= 1
                log.warning(
                    "clang-format reported issues in %d file(s)",
                    results.count(False),
                )

        # ---------------------------------------------------------------------------
        # CMake
        # ---------------------------------------------------------------------------
        log.info("Linting CMake")

        try:
            if fix:
                _format_cmake_files(cmake_root)
            if not _run(f"cmakelint {root}"):
                exit_code |= 1
                log.warning("cmakelint reported issues")
        except Exception:
            exit_code |= 1
            log.exception("CMake linting failed")

        if exit_code:
            log.warning("Linting completed with issues (exit_code=%d)", exit_code)
        else:
            log.info("Linting completed successfully")
