# aember/cli/base/test.py

from enum import Enum

import typer  # type: ignore

from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.cmake.build import BuildStep
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.resources.fetch_resources import FetchResources
from aember.steps.testing.coverage import CoverageReport, ResetCoverage
from aember.steps.testing.run_cli_tests import RunCliTests
from aember.steps.testing.run_tests import NATIVE_TEST_ARCHS, RunTests


class Sanitizer(str, Enum):
    asan = "asan"
    tsan = "tsan"


def test(
    filter: str = typer.Option(
        None, "--filter", "-k", help="Only suites whose name contains this"
    ),
    repeat: int = typer.Option(1, "--repeat", "-n", help="Run everything N times"),
    timeout: int = typer.Option(300, "--timeout", help="Seconds per suite"),
    cli: bool = typer.Option(
        True, "--cli/--no-cli", help="Also run the aember-cli Python tests"
    ),
    coverage: bool = typer.Option(
        False,
        "--coverage",
        help="Build with gcov counters, run, report per library "
        "(build/coverage/index.html)",
    ),
    sanitize: Sanitizer = typer.Option(
        None,
        "--sanitize",
        help="Build and run with asan (Address+Leak+UB sanitizers) or tsan",
        case_sensitive=False,
    ),
    valgrind: bool = typer.Option(
        False, "--valgrind", help="Run every suite under valgrind memcheck"
    ),
    root: bool = typer.Option(
        False,
        "--root",
        help="Run the suites as root (sudo), so root-only tests run too",
    ),
    arch: Architecture = typer.Option(
        Architecture.x64_linux.value,
        "--arch",
        "-a",
        help="x64-poky-linux, or x64-poky-linux-musl for the musl variant "
        "(both run on the build host)",
    ),
    build: bool = typer.Option(
        None,
        "--build/--no-build",
        help="Build the tests first (default: yes for --coverage/--sanitize, "
        "no for a plain run, which uses `aember qemu build --with-tests`)",
        show_default=False,
    ),
):
    """Run the aember-cli tests and the C++ unit tests.

    Plain runs use the x64 Debug build of `aember qemu build --with-tests`.
    --coverage and --sanitize build their own variant first
    (build/configure/x64-poky-linux/Debug-<variant>/).
    """
    if coverage and sanitize:
        raise typer.BadParameter("--coverage and --sanitize are separate builds")
    if valgrind and sanitize:
        raise typer.BadParameter("valgrind can't run sanitizer builds")
    instrument = "coverage" if coverage else (sanitize.value if sanitize else None)
    if arch.value not in NATIVE_TEST_ARCHS:
        raise typer.BadParameter(f"--arch must be one of {NATIVE_TEST_ARCHS}")
    if arch.value != Architecture.x64_linux.value and instrument:
        # GCC's sanitizers don't support musl; coverage stays on glibc.
        raise typer.BadParameter("--coverage and --sanitize are x64-poky-linux only")

    ctx = Context(
        architecture=arch.value,
        build_type=BuildType.debug.value,
        build_tests=True,
        instrument=instrument,
    )
    steps = []
    # -k filters C++ suites; the CLI tests run with it only for "cli".
    cli_tests = cli and (filter is None or filter in "aember-cli")
    if cli_tests:
        steps.append(RunCliTests())
    if build is None:
        build = instrument is not None
    if build:
        steps += [FetchResources(), ConfigureStep(), BuildStep()]
    if coverage:
        steps.append(ResetCoverage())
    steps.append(
        RunTests(
            filter=filter,
            repeat=repeat,
            timeout=timeout,
            valgrind=valgrind,
            as_root=root,
            # `-k cli` selects the CLI tests only: no C++ suite matching it
            # is fine then.
            allow_empty=cli_tests and filter is not None,
        )
    )
    if coverage:
        steps.append(CoverageReport())
    Pipeline(steps).run(ctx)
