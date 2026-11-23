import logging
from aember.config.sdk import VCPKG_TRIPLET_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner

from .sdk_build import build_dir, install_dir, sdk_shell

log = logging.getLogger("aember.configure")


class ConfigureStep(Step):

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

        vcpkg_installed_path = build_dir(ctx) / "vcpkg_installed"
        vcpkg_chainload_file = (
            ctx.root / "tools" / "toolchains" / f"{ctx.architecture}.cmake"
        )

        return [
            "cmake",
            f"-DARCHITECTURE={ctx.architecture}",
            f"-DCMAKE_BUILD_TYPE={ctx.build_type}",
            f"-DCMAKE_INSTALL_PREFIX={install_dir(ctx)}",
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain}",
            f"-DVCPKG_TARGET_TRIPLET={triplet}",
            f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={vcpkg_chainload_file}",
            # triplets not in the vcpkg submodule (arm-poky-linux)
            f"-DVCPKG_OVERLAY_TRIPLETS={ctx.root / 'tools' / 'triplets'}",
            f"-DVCPKG_INSTALLED_DIR={vcpkg_installed_path}",
            f"-DBUILD_TESTS={ctx.build_tests}",
            f"-DAEMBER_INSTRUMENT={ctx.instrument or ''}",
            "-DVCPKG_MANIFEST_MODE=ON",
            "-B",
            str(build_dir(ctx)),
            "-S",
            str(ctx.root),
        ]

    # ---- Execution ----------------------------------------------------

    def ensure_vcpkg(self, ctx: Context) -> None:
        """Check out the vcpkg submodule if a plain `git clone` left it empty."""
        toolchain = ctx.root / "vcpkg" / "scripts" / "buildsystems" / "vcpkg.cmake"
        if toolchain.exists():
            return
        log.info("vcpkg submodule missing — running git submodule update")
        CommandRunner.run(
            ["git", "-C", str(ctx.root), "submodule", "update", "--init", "vcpkg"]
        )

    def run(self, ctx: Context) -> None:
        log.info("Configuring project (%s / %s)", ctx.architecture, ctx.variant)
        self.ensure_vcpkg(ctx)
        build_dir(ctx).mkdir(parents=True, exist_ok=True)
        ctx.data["build_dir"] = build_dir(ctx)
        ctx.data["install_dir"] = install_dir(ctx)
        CommandRunner.run(sdk_shell(ctx, self.cmake_command(ctx)), shell=True)
        log.info("Configure step completed successfully")
