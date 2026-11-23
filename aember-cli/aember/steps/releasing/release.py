import logging
import tarfile
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step

from .get_version_from_cmake import get_version_from_cmake
from aember.utils.download import sha256_file

log = logging.getLogger("aember.release")

# GitHub Releases file size limit
GITHUB_MAX_BYTES = 2 * 1024 * 1024 * 1024  # 2GB


class ReleaseStep(Step):
    """
    Pack aember's own release artifacts into build/release/<arch>/<type>/.

    Kernel, BusyBox, LXC and the SDK are not released here: they come from
    the aember-resources releases (see aember/config/resources.py).
    """

    def _tar(self, tar_path: Path, add) -> None:
        with tarfile.open(tar_path, "w:gz") as tar:
            add(tar)
        size = tar_path.stat().st_size
        if size > GITHUB_MAX_BYTES:
            raise RuntimeError(
                f"{tar_path.name} exceeds the GitHub 2 GB limit "
                f"({size / 2**20:.0f} MB)"
            )
        log.info("  ✅ %s (%.1f MB)", tar_path.name, size / 2**20)

    def _pack_resources(self, root: Path, release_dir: Path, version: str) -> None:
        """Pack the resources/ rootfs overlay (both architectures)."""
        resources_dir = root / "resources"
        if not resources_dir.exists():
            log.warning("No resources directory found — skipping")
            return

        artifact = release_dir / f"aember-resources-v{version}.tar.gz"
        log.info("Packing resources → %s", artifact.name)
        self._tar(
            artifact,
            lambda tar: tar.add(
                resources_dir,
                arcname="resources",
                filter=lambda t: None if t.name.endswith(".gitkeep") else t,
            ),
        )
        sha256 = sha256_file(artifact)
        (release_dir / f"{artifact.name}.sha256").write_text(sha256 + "\n")
        log.info("Resources hash: %s", sha256)

    def run(self, ctx: Context) -> None:
        log.info(
            "Creating release artifacts for '%s' / '%s'",
            ctx.architecture,
            ctx.build_type,
        )

        root = ctx.root
        cmake_file = root / "CMakeLists.txt"
        if not cmake_file.exists():
            raise FileNotFoundError(f"CMakeLists.txt not found at {cmake_file}")

        version = get_version_from_cmake(str(cmake_file))
        log.info("Version: %s", version)

        arch = ctx.architecture
        build_type = ctx.build_type
        release_dir = root / "build" / "release" / arch / build_type
        release_dir.mkdir(parents=True, exist_ok=True)

        # Resources (shared across architectures/build types)
        self._pack_resources(root, release_dir, version)

        # Aember app package (initramfs + kernel image + rootfs image)
        package_dir = root / "build" / "package" / arch / build_type
        if package_dir.exists():
            log.info("Packing aember app package")
            self._tar(
                release_dir / f"aember-app-{arch}-{build_type}-v{version}.tar.gz",
                lambda tar: [
                    tar.add(f, arcname=f.name)
                    for f in package_dir.iterdir()
                    if f.name != "initramfs_root"  # skip unpacked tree
                ],
            )
        else:
            log.warning("Package dir not found: %s", package_dir)

        # Aember binary only (for users who want just the executable)
        aember_bin = root / "build" / "install" / arch / build_type / "bin" / "aember"
        if aember_bin.exists():
            log.info("Packing aember binary")
            self._tar(
                release_dir / f"aember-binary-{arch}-{build_type}-v{version}.tar.gz",
                lambda tar: tar.add(aember_bin, arcname="aember"),
            )

        self._log_summary(release_dir, version)

    def _log_summary(self, release_dir: Path, version: str) -> None:
        log.info("Release artifacts written to: %s", release_dir)
        log.info("Upload to GitHub Releases v%s:", version)
        total_mb = 0.0
        for f in sorted(release_dir.iterdir()):
            size_mb = f.stat().st_size / (1024 * 1024)
            total_mb += size_mb
            log.info("  📦 %s (%.1f MB)", f.name, size_mb)
        log.info("Total: %.1f MB", total_mb)
