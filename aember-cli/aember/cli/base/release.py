# aember/cli/release.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.cmake.build import BuildStep
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.packaging.pack import PackStep
from aember.steps.releasing.images import ReleaseImages
from aember.steps.releasing.release import ReleaseStep
from aember.steps.resources.check_resources import CheckResources
from aember.steps.resources.fetch_resources import FetchResources


def release(
    architectures: list[Architecture] = typer.Option(
        list(Architecture),
        "--arch",
        "-a",
        help="Architectures to release (repeatable; default: all)",
        case_sensitive=False,
    ),
):
    for arch in architectures:
        for bt in BuildType:
            # Default rootfs size: the overlay's container images alone are
            # ~180 MB, so the old 128 MB image no longer fits.
            ctx = Context(architecture=arch.value, build_type=bt.value)

            Pipeline(
                [
                    CheckResources(),
                    FetchResources(),
                    ConfigureStep(),
                    BuildStep(),
                    PackStep(),
                    ReleaseStep(),
                    # Release builds: the Pi 4 SD card image (aarch64) and
                    # the x86-64 USB stick image, compressed.
                    ReleaseImages(),
                ]
            ).run(ctx)
