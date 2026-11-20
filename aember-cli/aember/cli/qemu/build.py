# aember/cli/release.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.cmake.build import BuildStep
from aember.steps.clean.clean import CleanStep
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.resources.check_resources import CheckResources
from aember.steps.resources.fetch_resources import FetchResources
from aember.steps.packaging.pack import PackStep


def build_qemu(
    architecture: Architecture = typer.Option(
        Architecture.x64_linux,
        help="Target architecture",
        case_sensitive=False,
    ),
    build_type: BuildType = typer.Option(
        BuildType.release,
        help="Build type",
        case_sensitive=False,
    ),
    build_tests: bool = typer.Option(
        False,
        "--with-tests/--no-tests",
        help="Enable unit tests (forces x64 Debug build type)",
    ),
):
    # Tests always require x64 Debug
    if build_tests:
        architecture = Architecture.x64_linux
        build_type = BuildType.debug

    ctx = Context(
        architecture=architecture.value,
        build_type=build_type.value,
        build_tests=build_tests,
    )

    steps = []

    # Clean only when building tests: a fresh initramfs with the latest test
    # binaries. (This used to read `build_tests or BuildType.debug`, which is
    # always true, so every build started by cleaning.)
    if build_tests:
        steps.append(CleanStep())

    steps += [
        CheckResources(),
        FetchResources(),
        ConfigureStep(),
        BuildStep(),
        PackStep(),
    ]

    Pipeline(steps).run(ctx)
