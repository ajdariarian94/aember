# aember/cli/base/appimage.py

import typer  # type: ignore

from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.appimage.build import AppImageStep


def appimage(
    build_type: BuildType = typer.Option(
        BuildType.release, help="Which packed x64 build to put in it"
    ),
):
    """x86-64 AppImage: runs aember isolated on a Linux desktop."""
    ctx = Context(architecture="x64-poky-linux", build_type=build_type.value)
    Pipeline([AppImageStep()]).run(ctx)
