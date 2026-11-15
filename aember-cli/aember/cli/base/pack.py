# aember/cli/pack.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.packaging.pack import PackStep


def pack(
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
        help="Enable unit tests",
    ),
):
    ctx = Context(
        architecture=architecture.value,
        build_type=build_type.value,
        build_tests=build_tests,
    )

    Pipeline(
        [
            PackStep(),
        ]
    ).run(ctx)
