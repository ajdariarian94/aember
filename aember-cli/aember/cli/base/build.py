# aember/cli/base/build.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.cmake.build import BuildStep
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.resources.check_resources import CheckResources
from aember.steps.resources.fetch_resources import FetchResources


def build(
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

    # Everything a fresh clone needs: overlay images, kernel/BusyBox/LXC/SDK
    # from aember-resources, CMake configure (vcpkg deps), then the build.
    # Each step skips work that is already done.
    Pipeline(
        [
            CheckResources(),
            FetchResources(),
            ConfigureStep(),
            BuildStep(),
        ]
    ).run(ctx)
