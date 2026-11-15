# aember/cli/launch.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.steps.launching.launch import LaunchStep


def launch(
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
):
    ctx = Context(architecture=architecture.value, build_type=build_type.value)
    LaunchStep().run(ctx)
