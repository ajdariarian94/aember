# aember/cli/base/fuzz.py

import typer  # type: ignore
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.resources.fetch_resources import FetchResources
from aember.steps.testing.fuzz import FuzzStep, targets


def fuzz(
    target: list[str] = typer.Option(
        None,
        "--target",
        "-k",
        help="Only targets whose name contains this (repeatable)",
    ),
    seconds: int = typer.Option(60, "--time", help="Seconds per target"),
    parallel: int = typer.Option(
        None, "--parallel", "-j", help="Targets at once (default: up to 4)"
    ),
    list_only: bool = typer.Option(False, "--list", help="List the targets and exit"),
):
    """Fuzz the code that reads input (configs, daemon channel, control
    socket, DHCP, status file) with libFuzzer, ASan and UBSan. Crashes:
    build/fuzz/crashes/<target>/; reproduce with build/fuzz/build/<target>
    <file>."""
    ctx = Context(
        architecture="x64-poky-linux",
        build_type=BuildType.debug.value,
        build_tests=True,
    )
    if list_only:
        for t in targets(ctx.root):
            typer.echo(t)
        return
    Pipeline([FetchResources(), FuzzStep(target or None, seconds, parallel)]).run(ctx)
