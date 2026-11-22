"""
`aember usb image`: a bootable x86-64 USB stick image from the packed x64
system (UEFI).

    build/usb/aember-x64-usb-v<V>.img      GPT
      p1  ESP, FAT32 AEMBERBOOT   EFI/BOOT/BOOTX64.EFI (systemd-boot)
                                  loader/loader.conf, loader/entries/*.conf
                                  aember/bzImage, aember/aember-initramfs.cpio.gz
      p2  ext4 aember-root        the same tree as the QEMU rootfs image

Flashing copies it to the start of the stick (`aember usb flash`, dd,
balenaEtcher, Raspberry Pi Imager "Use custom").
"""

import logging
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

from aember.config import usb
from aember.config.host_path import HOST_ROOT_PATH
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.appimage.build import fetch_pinned
from aember.steps.packaging.rootfs import SIZE_EXTRA_MB, SIZE_MARGIN, _tree_size
from aember.steps.packaging.rootfs import run_guestfish, tar_in_commands
from aember.steps.releasing.get_version_from_cmake import get_version_from_cmake

log = logging.getLogger("aember.usb.image")


def image_path(root: Path) -> Path:
    version = get_version_from_cmake(str(root / "CMakeLists.txt"))
    return root / "build" / "usb" / f"aember-x64-usb-v{version}.img"


def systemd_boot_efi(root: Path) -> Path:
    """systemd-bootx64.efi from the pinned Ubuntu package (cached)."""
    cache = root / "build" / "downloads" / "systemd-boot"
    deb = fetch_pinned(usb.SYSTEMD_BOOT_URL, usb.SYSTEMD_BOOT_SHA256, cache)
    efi = cache / "systemd-bootx64.efi"
    if efi.exists() and efi.stat().st_mtime >= deb.stat().st_mtime:
        return efi
    with tempfile.TemporaryDirectory(dir=cache) as tmp:
        subprocess.run(["dpkg-deb", "-x", str(deb), tmp], check=True)
        shutil.copy2(Path(tmp) / usb.SYSTEMD_BOOT_EFI, efi)
    return efi


def write_esp_tree(esp: Path, package_dir: Path, efi: Path, version: str) -> None:
    """The EFI system partition's files, in @p esp."""
    if esp.exists():
        shutil.rmtree(esp)
    (esp / "EFI" / "BOOT").mkdir(parents=True)
    shutil.copy2(efi, esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    (esp / "aember").mkdir()
    shutil.copy2(package_dir / "kernel", esp / usb.KERNEL)
    shutil.copy2(package_dir / "aember-initramfs.cpio.gz", esp / usb.INITRAMFS)
    entries = esp / "loader" / "entries"
    entries.mkdir(parents=True)
    (esp / "loader" / "loader.conf").write_text(usb.LOADER_CONF)
    for i, (name, title, options) in enumerate(usb.ENTRIES, start=1):
        (entries / name).write_text(
            usb.entry_conf(title, version, options, f"aember-{i}")
        )


class UsbImageStep(Step):
    def run(self, ctx: Context) -> None:
        root = ctx.root
        package_dir = root / "build" / "package" / usb.ARCHITECTURE / ctx.build_type
        initramfs_root = package_dir / "initramfs_root"
        services_root = package_dir / "services_root"
        # Opt-in packages (packages.json) and the user database.
        packages_root = package_dir / "packages_root"
        # The overlay as `aember app pack` staged it: only listed containers.
        overlay = package_dir / "overlay_root"
        for needed in (package_dir / "kernel", initramfs_root, overlay):
            if not needed.exists():
                raise FileNotFoundError(
                    f"{needed} not found: run `aember app build` and `aember app "
                    f"pack --architecture {usb.ARCHITECTURE}` first."
                )
        version = get_version_from_cmake(str(root / "CMakeLists.txt"))

        esp = root / "build" / "usb" / "esp"
        write_esp_tree(esp, package_dir, systemd_boot_efi(root), version)
        esp_mb = usb.ESP_SECTORS // 2048
        if _tree_size(esp) > (esp_mb - 16) * 1024 * 1024:
            raise RuntimeError(f"boot files don't fit {esp_mb} MiB")

        root_mb = (
            math.ceil(
                (
                    _tree_size(initramfs_root)
                    + _tree_size(services_root)
                    + _tree_size(packages_root)
                    + _tree_size(overlay)
                )
                / (1024 * 1024)
                * SIZE_MARGIN
            )
            + SIZE_EXTRA_MB
        )
        # + 1 MiB for the backup GPT at the end.
        total_mb = usb.ESP_START // 2048 + esp_mb + root_mb + 1

        img = image_path(root)
        img.parent.mkdir(parents=True, exist_ok=True)
        for old in img.parent.glob("aember-x64-usb-v*.img"):
            if old != img:
                log.info("Removing old image %s", old)
                old.unlink()
        img.unlink(missing_ok=True)
        with img.open("wb") as f:
            f.truncate(total_mb * 1024 * 1024)

        esp_end = usb.ESP_START + usb.ESP_SECTORS - 1
        tars = root / "build" / "usb" / "rootfs-tars"
        script = [
            f"add {img}",
            "run",
            "part-init /dev/sda gpt",
            f"part-add /dev/sda p {usb.ESP_START} {esp_end}",
            f"part-add /dev/sda p {esp_end + 1} -34",
            f"part-set-gpt-type /dev/sda 1 {usb.ESP_TYPE}",
            "part-set-name /dev/sda 1 EFI",
            "part-set-name /dev/sda 2 aember-root",
            f"mkfs vfat /dev/sda1 label:{usb.BOOT_LABEL}",
            f"mkfs ext4 /dev/sda2 label:{usb.ROOT_LABEL}",
            "mount /dev/sda2 /",
            *tar_in_commands(
                [initramfs_root, services_root, packages_root, overlay], tars
            ),
            "umount /",
            "mount /dev/sda1 /",
            *[f"copy-in {e} /" for e in esp.iterdir()],
            "umount /",
            "exit",
        ]
        log.info("Building %s (%d MiB)", img, total_mb)
        try:
            run_guestfish(script, what=str(img))
        finally:
            shutil.rmtree(tars, ignore_errors=True)
        shutil.rmtree(esp, ignore_errors=True)

        log.info("x86-64 USB image: %s", img)
        host_path = str(HOST_ROOT_PATH) + str(img).removeprefix(str(root))
        log.info(
            'Write it with balenaEtcher or Raspberry Pi Imager ("Use custom"); '
            "from Windows: \\\\wsl.localhost\\<distro>%s . Or attach the stick to "
            "WSL (usbipd) and run `aember usb flash --device /dev/sdX`. Boot the "
            "PC from it in UEFI mode with Secure Boot off.",
            host_path.replace("/", "\\"),
        )
        ctx.data["usb_image"] = img
