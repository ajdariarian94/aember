import logging
from pathlib import Path

from aember.config.resources import sdk_dir
from aember.config.sdk import SDK_ENV_MAP, VCPKG_TRIPLET_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner

log = logging.getLogger("aember.configure")


class ConfigureStep(Step):

    # ---- Derived paths -------------------------------------------------

    def build_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "configure" / ctx.architecture / ctx.build_type

    def install_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "install" / ctx.architecture / ctx.build_type

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
        vcpkg_root = ctx.root / "vcpkg"
        vcpkg_toolchain = vcpkg_root / "scripts" / "buildsystems" / "vcpkg.cmake"

        triplet = VCPKG_TRIPLET_MAP.get(ctx.architecture)
        if not triplet:
            raise ValueError(
                f"No vcpkg triplet mapping for architecture: '{ctx.architecture}'. "
                f"Known: {list(VCPKG_TRIPLET_MAP)}"
            )

        build_dir = self.build_dir(ctx)
        vcpkg_installed_path = build_dir / "vcpkg_installed"
        vcpkg_chainload_file = (
            ctx.root / "tools" / "toolchains" / f"{ctx.architecture}.cmake"
        )

        return [
            "cmake",
            f"-DARCHITECTURE={ctx.architecture}",
            f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
            f"-DCMAKE_INSTALL_PREFIX={self.install_dir(ctx)}",
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain}",
            f"-DVCPKG_TARGET_TRIPLET={triplet}",
            f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={vcpkg_chainload_file}",
            f"-DVCPKG_INSTALLED_DIR={vcpkg_installed_path}",
            f"-DBUILD_TESTS={ctx.build_tests}",
            "-DVCPKG_MANIFEST_MODE=ON",
            "-B",
            str(build_dir),
            "-S",
            str(ctx.root),
        ]

    # ---- Execution ----------------------------------------------------

    def run(self, ctx: Context) -> None:
        log.info("Configuring project")
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

        full_cmd = f"""
            source {sdk_env} &&
            unset CC &&
            unset CXX &&
            {cmake_cmd}
        """

        log.info("Running CMake configure step")
        CommandRunner.run(full_cmd, shell=True)
        log.info("Configure step completed successfully")
