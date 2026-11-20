import logging
from pathlib import Path

from aember.config.build import JOBS_ENV, build_jobs
from aember.config.resources import sdk_dir
from aember.config.sdk import SDK_ENV_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner
from aember.utils.merge_compile_command import CompileCommandsMerger

log = logging.getLogger("aember.build")


class BuildStep(Step):

    # ---- Derived paths -------------------------------------------------

    def build_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "configure" / ctx.architecture / ctx.variant

    def install_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "install" / ctx.architecture / ctx.variant

    def toolchain_file(self, ctx: Context) -> Path:
        return ctx.root / "tools" / "toolchains" / f"{ctx.architecture}.cmake"

    def sdk_env(self, ctx: Context) -> Path:
        env_name = SDK_ENV_MAP.get(ctx.architecture)
        if not env_name:
            raise ValueError(
                f"No SDK environment mapping for architecture: '{ctx.architecture}'. "
                f"Known: {list(SDK_ENV_MAP)}"
            )
        return sdk_dir(ctx.root, ctx.architecture) / env_name

    # ---- Command construction -----------------------------------------

    def cmake_command(self, ctx: Context) -> list[str]:
        cmd = [
            "cmake",
            "--build",
            str(self.build_dir(ctx)),
            "--parallel",
            str(build_jobs()),
        ]
        log.debug("CMake build command: %s", " ".join(cmd))
        return cmd

    # ---- Execution ----------------------------------------------------

    def run(self, ctx: Context) -> None:
        log.info("Building project")
        log.debug("Architecture: %s", ctx.architecture)
        log.debug("Build type:   %s", ctx.build_type)

        build_dir = self.build_dir(ctx)
        install_dir = self.install_dir(ctx)

        log.debug("Build directory:   %s", build_dir)
        log.debug("Install directory: %s", install_dir)

        build_dir.mkdir(parents=True, exist_ok=True)

        ctx.data["build_dir"] = build_dir
        ctx.data["install_dir"] = install_dir

        sdk_env = self.sdk_env(ctx)
        cmake_cmd = " ".join(self.cmake_command(ctx))
        # WSL crashes when a build uses every core (config/build.py)
        jobs = build_jobs()

        full_cmd = f"""
            source {sdk_env} &&
            unset CC &&
            unset CXX &&
            export {JOBS_ENV}={jobs} VCPKG_MAX_CONCURRENCY={jobs} &&
            {cmake_cmd}
        """

        log.info("Running CMake build step")
        CommandRunner.run(full_cmd, shell=True)

        log.info("Merging compile_commands.json for clangd")
        CompileCommandsMerger.merge(ctx)

        log.info("Build step completed successfully")
