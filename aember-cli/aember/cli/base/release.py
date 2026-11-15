# aember/cli/release.py

from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.cmake.build import BuildStep
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.packaging.pack import PackStep
from aember.steps.releasing.release import ReleaseStep


def release():
    for arch in Architecture:
        for bt in BuildType:
            ctx = Context(architecture=arch.value, build_type=bt.value, rootfs_size=128)

            Pipeline(
                [
                    ConfigureStep(),
                    BuildStep(),
                    PackStep(),
                    ReleaseStep(),
                ]
            ).run(ctx)
