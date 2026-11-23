import logging
from aember.config.build import build_jobs
from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner
from aember.utils.merge_compile_command import CompileCommandsMerger

from .sdk_build import build_dir, install_dir, sdk_shell

log = logging.getLogger("aember.build")


class BuildStep(Step):
    def cmake_command(self, ctx: Context) -> list[str]:
        return [
            "cmake",
            "--build",
            str(build_dir(ctx)),
            "--parallel",
            str(build_jobs()),
        ]

    def run(self, ctx: Context) -> None:
        log.info("Building project (%s / %s)", ctx.architecture, ctx.variant)
        build_dir(ctx).mkdir(parents=True, exist_ok=True)
        ctx.data["build_dir"] = build_dir(ctx)
        ctx.data["install_dir"] = install_dir(ctx)
        CommandRunner.run(sdk_shell(ctx, self.cmake_command(ctx)), shell=True)
        log.info("Merging compile_commands.json for clangd")
        CompileCommandsMerger.merge(ctx)
        log.info("Build step completed successfully")
