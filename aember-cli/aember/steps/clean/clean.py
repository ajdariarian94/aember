import logging
import shutil

from aember.core.context import Context
from aember.core.step import Step

from .chroot import cleanup_chroot

log = logging.getLogger("aember.clean")


class CleanStep(Step):

    def run(self, ctx: Context) -> None:
        log.info("Cleaning build artifacts for %s/%s", ctx.architecture, ctx.build_type)

        build_root = ctx.root / "build"

        # Unmount initramfs mounts for the specified architecture and build type.
        for name in ("package", "install", "release"):
            initramfs_root = (
                build_root / name / ctx.architecture / ctx.build_type / "initramfs_root"
            )
            if initramfs_root.exists():
                log.debug("Cleaning initramfs chroot: %s", initramfs_root)
                cleanup_chroot(initramfs_root)

        # Remove build output dirs for the specified architecture and build type.
        for name in ("package", "install", "release"):
            d = build_root / name / ctx.architecture / ctx.build_type
            if d.exists():
                log.info("Removing: %s", d)
                shutil.rmtree(d, ignore_errors=True)

        log.info("Clean step completed successfully")
