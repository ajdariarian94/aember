"""
`aember rpi image`: a Raspberry Pi 4 SD card image from the packed aarch64
system.

    build/rpi/aember-rpi4-v<V>.img
      p1  FAT32 (0x0c) AEMBERBOOT  start4.elf fixup4.dat LICENCE.broadcom
                                   config.txt cmdline.txt kernel8.img
                                   aember-initramfs.cpio.gz *.dtb
      p2  ext4 aember-root         the same tree as the QEMU rootfs image

The image size is a power of two (QEMU's raspi machines require that for
-sd); flashing copies it to the start of the card.
"""

import logging
import math
import shutil
from pathlib import Path

from aember.config import rpi
from aember.config.host_path import HOST_ROOT_PATH
from aember.config.resources import kernel_dir
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.packaging.rootfs import SIZE_EXTRA_MB, SIZE_MARGIN, _tree_size
from aember.steps.packaging.rootfs import run_guestfish, tar_in_commands
from aember.steps.releasing.get_version_from_cmake import get_version_from_cmake
from aember.utils.download import fetch_pinned

log = logging.getLogger("aember.rpi.image")


def fetch_firmware(root: Path) -> Path:
    """Pinned Raspberry Pi firmware files, downloaded once and verified."""
    dest = root / "build" / "downloads" / "rpi-firmware" / rpi.FIRMWARE_TAG
    for name, sha in rpi.FIRMWARE_FILES.items():
        url = rpi.FIRMWARE_URL.format(tag=rpi.FIRMWARE_TAG, name=name)
        fetch_pinned(url, sha, dest, name)
    return dest


def image_path(root: Path) -> Path:
    version = get_version_from_cmake(str(root / "CMakeLists.txt"))
    return root / "build" / "rpi" / f"aember-rpi4-v{version}.img"


class RpiImageStep(Step):
    def run(self, ctx: Context) -> None:
        root = ctx.root
        package_dir = root / "build" / "package" / rpi.ARCHITECTURE / ctx.build_type
        initramfs_root = package_dir / "initramfs_root"
        # Services (aember-netd, aember-containerd, ...) are rootfs-only.
        services_root = package_dir / "services_root"
        # Opt-in packages (packages.json) and the user database.
        packages_root = package_dir / "packages_root"
        notices_root = package_dir / "notices_root"  # third-party notices
        # The overlay as `aember app pack` staged it: only listed containers.
        overlay = package_dir / "overlay_root"
        for needed in (package_dir / "kernel", initramfs_root, overlay):
            if not needed.exists():
                raise FileNotFoundError(
                    f"{needed} not found: run `aember app build` and `aember app "
                    f"pack --architecture {rpi.ARCHITECTURE}` first."
                )
        dtb_dir = kernel_dir(root, rpi.ARCHITECTURE) / "dtb"
        if not (dtb_dir / rpi.DTB_PI4).exists():
            raise FileNotFoundError(
                f"{dtb_dir / rpi.DTB_PI4} not found: the aember-resources kernel "
                f"asset has no Raspberry Pi device tree (needs v0.1.5+)."
            )

        # Boot partition contents.
        boot = root / "build" / "rpi" / "boot"
        if boot.exists():
            shutil.rmtree(boot)
        boot.mkdir(parents=True)
        for f in fetch_firmware(root).iterdir():
            shutil.copy2(f, boot)
        shutil.copy2(package_dir / "kernel", boot / "kernel8.img")
        shutil.copy2(package_dir / "aember-initramfs.cpio.gz", boot)
        for dtb in (rpi.DTB_PI4, rpi.DTB_PI3):
            if (dtb_dir / dtb).exists():
                shutil.copy2(dtb_dir / dtb, boot)
        (boot / "config.txt").write_text(rpi.CONFIG_TXT.format(dtb=rpi.DTB_PI4))
        (boot / "cmdline.txt").write_text(rpi.CMDLINE_TXT)

        boot_mb = rpi.BOOT_SECTORS // 2048
        if _tree_size(boot) > (boot_mb - 16) * 1024 * 1024:
            raise RuntimeError(f"boot files don't fit {boot_mb} MiB")

        # Root partition size from its contents; total a power of two MiB.
        root_mb = (
            math.ceil(
                (
                    _tree_size(initramfs_root)
                    + _tree_size(services_root)
                    + _tree_size(packages_root)
                    + _tree_size(notices_root)
                    + _tree_size(overlay)
                )
                / (1024 * 1024)
                * SIZE_MARGIN
            )
            + SIZE_EXTRA_MB
        )
        start_mb = rpi.BOOT_START // 2048
        total_mb = 1 << math.ceil(math.log2(start_mb + boot_mb + root_mb))

        img = image_path(root)
        img.parent.mkdir(parents=True, exist_ok=True)
        # One image at a time: older versions' images are GBs nobody uses.
        for old in img.parent.glob("aember-rpi4-v*.img"):
            if old != img:
                log.info("Removing old image %s", old)
                old.unlink()
        with img.open("wb") as f:
            f.truncate(total_mb * 1024 * 1024)

        boot_end = rpi.BOOT_START + rpi.BOOT_SECTORS - 1
        tars = root / "build" / "rpi" / "rootfs-tars"
        script = [
            f"add {img}",
            "run",
            "part-init /dev/sda mbr",
            f"part-add /dev/sda p {rpi.BOOT_START} {boot_end}",
            f"part-add /dev/sda p {boot_end + 1} -1",
            "part-set-mbr-id /dev/sda 1 0x0c",
            "part-set-bootable /dev/sda 1 true",
            f"mkfs vfat /dev/sda1 label:{rpi.BOOT_LABEL}",
            f"mkfs ext4 /dev/sda2 label:{rpi.ROOT_LABEL}",
            "mount /dev/sda2 /",
            *tar_in_commands(
                [initramfs_root, services_root, packages_root, notices_root, overlay],
                tars,
            ),
            "umount /",
            "mount /dev/sda1 /",
            *[f"copy-in {e} /" for e in boot.iterdir()],
            "umount /",
            "exit",
        ]
        log.info("Building %s (%d MiB)", img, total_mb)
        try:
            run_guestfish(script, what=str(img))
        finally:
            shutil.rmtree(tars, ignore_errors=True)

        log.info("Raspberry Pi 4 image: %s", img)
        host_path = str(HOST_ROOT_PATH) + str(img).removeprefix(str(root))
        log.info(
            'Write it with Raspberry Pi Imager or balenaEtcher ("Use custom"); '
            "from Windows: \\\\wsl.localhost\\<distro>%s . Or attach the card "
            "reader to WSL (usbipd) and run `aember rpi flash --device /dev/sdX`.",
            host_path.replace("/", "\\"),
        )
        ctx.data["rpi_image"] = img
