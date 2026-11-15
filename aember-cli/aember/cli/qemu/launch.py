# aember/cli/launch.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.steps.launching.launch import LaunchStep


def launch_qemu(
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
    headless: bool = typer.Option(
        False,
        "--headless",
        help="Run in this terminal without windows (serial console on stdout)",
    ),
    timeout: int = typer.Option(
        None,
        "--timeout",
        help="With --headless: stop QEMU after this many seconds",
    ),
):
    ctx = Context(architecture=architecture.value, build_type=build_type.value)
    ctx.data.update(headless=headless, timeout=timeout)
    LaunchStep().run(ctx)
