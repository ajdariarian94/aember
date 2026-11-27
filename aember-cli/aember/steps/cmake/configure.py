import logging
import re
import shutil
from pathlib import Path

from aember.config.sdk import SDK_SYSROOT_MAP, VCPKG_TRIPLET_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner

from .sdk_build import build_dir, install_dir, sdk_shell

log = logging.getLogger("aember.configure")

# The SDK's host part: its sysroot shows up in every compiler probe.
SDK_HOST_SYSROOT = "x86_64-pokysdk-linux"

_PROBE_DIRS = re.compile(r"set\(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES \"([^\"]*)\"\)")


def probe_sysroots(probe_file: Path) -> set[str]:
    """The SDK sysroot names a cached compiler probe
    (CMakeFiles/<v>/CMakeCXXCompiler.cmake) has in its implicit include
    directories."""
    match = _PROBE_DIRS.search(probe_file.read_text(errors="replace"))
    if not match:
        return set()
    names = set()
    for directory in match.group(1).split(";"):
        parts = Path(directory).parts
        if "sysroots" in parts and parts.index("sysroots") + 1 < len(parts):
            names.add(parts[parts.index("sysroots") + 1])
    return names


def stale_probe(directory: Path, sysroot: str) -> Path | None:
    """A compiler probe under @p directory made with another SDK sysroot than
    @p sysroot, or None. Such a build directory no longer counts
    <sysroot>/usr/include as implicit, and a package reporting it (FindLibLZMA)
    then breaks libstdc++'s #include_next (tools/cmake/AemberSysrootCheck.cmake).
    """
    if not directory.is_dir():
        return None
    for probe in sorted(directory.glob("**/CMakeFiles/*/CMakeCXXCompiler.cmake")):
        other = probe_sysroots(probe) - {sysroot, SDK_HOST_SYSROOT}
        if other:
            return probe
    return None


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

    def drop_stale_build_dir(self, ctx: Context) -> None:
        """Remove the build directory when its compiler probe comes from
        another SDK (the whole directory: with only the caches gone,
        ExternalProject would skip its configure). vcpkg's binary cache makes
        the rebuild quick."""
        sysroot = SDK_SYSROOT_MAP.get(ctx.architecture)
        probe = stale_probe(build_dir(ctx), sysroot) if sysroot else None
        if probe is None:
            return
        log.warning(
            "%s was configured with another SDK sysroot (%s); removing %s and "
            "configuring again",
            build_dir(ctx),
            ", ".join(sorted(probe_sysroots(probe) - {sysroot, SDK_HOST_SYSROOT})),
            build_dir(ctx),
        )
        shutil.rmtree(build_dir(ctx))

    def run(self, ctx: Context) -> None:
        log.info("Configuring project (%s / %s)", ctx.architecture, ctx.variant)
        self.ensure_vcpkg(ctx)
        self.drop_stale_build_dir(ctx)
        build_dir(ctx).mkdir(parents=True, exist_ok=True)
        ctx.data["build_dir"] = build_dir(ctx)
        ctx.data["install_dir"] = install_dir(ctx)
        CommandRunner.run(sdk_shell(ctx, self.cmake_command(ctx)), shell=True)
        log.info("Configure step completed successfully")
