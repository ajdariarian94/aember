"""
Bootable x86-64 USB image (`aember usb image`): UEFI, GPT, systemd-boot.

The PC's UEFI firmware starts \\EFI\\BOOT\\BOOTX64.EFI from the stick's EFI
system partition: systemd-boot, which shows a short menu and starts the
kernel (EFI stub) with the initramfs. Root is the ext4 second partition,
found by its label (aember has no udev, RootManager reads the superblocks;
the USB stick may take a few seconds to show up, it waits up to 30 s).

UEFI only (no legacy BIOS): every x86-64 PC since ~2012 boots it. Secure
Boot must be off (the kernel is not signed).
"""

ARCHITECTURE = "x64-poky-linux"

# systemd-boot from Ubuntu 24.04's release pocket (never removed from the
# archive), verified by sha256; only its EFI binary is used.
SYSTEMD_BOOT_URL = (
    "http://archive.ubuntu.com/ubuntu/pool/universe/s/systemd/"
    "systemd-boot-efi_255.4-1ubuntu8_amd64.deb"
)
SYSTEMD_BOOT_SHA256 = "c33d8a65728cda682569317bdd0e7231fc8538346a3aa4ca641c0e8786665e87"
SYSTEMD_BOOT_EFI = "usr/lib/systemd/boot/efi/systemd-bootx64.efi"

ROOT_LABEL = "aember-root"
BOOT_LABEL = "AEMBERBOOT"
# GPT type of an EFI system partition.
ESP_TYPE = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

# GPT layout in 512-byte sectors: the ESP (FAT32) at 1 MiB, 256 MiB.
ESP_START = 2048
ESP_SECTORS = 256 * 2048

# Files on the ESP.
KERNEL = "aember/bzImage"
INITRAMFS = "aember/aember-initramfs.cpio.gz"

ROOT_ARGS = f"root=LABEL={ROOT_LABEL} rootfstype=ext4 rootwait rw"

# The last console= is /dev/console (aember's log and the `d` debug shell).
# (file name, title, kernel arguments); the first entry is the default.
ENTRIES: list[tuple[str, str, str]] = [
    (
        "aember.conf",
        "aember",
        f"console=ttyS0,115200 console=tty0 {ROOT_ARGS}",
    ),
    (
        "aember-serial.conf",
        "aember (serial console)",
        f"console=tty0 console=ttyS0,115200 {ROOT_ARGS}",
    ),
    (
        "aember-shell.conf",
        "aember (debug shell at boot)",
        f"console=ttyS0,115200 console=tty0 {ROOT_ARGS} aember.shell=1",
    ),
]

LOADER_CONF = """\
# aember USB stick (written by `aember usb image`)
default aember.conf
timeout 3
console-mode keep
editor yes
"""


def entry_conf(title: str, version: str, options: str, sort_key: str) -> str:
    """A systemd-boot loader entry (Boot Loader Specification type 1)."""
    return (
        f"title {title}\n"
        f"version {version}\n"
        f"sort-key {sort_key}\n"
        f"linux /{KERNEL}\n"
        f"initrd /{INITRAMFS}\n"
        f"options {options}\n"
    )


# `aember usb flash` refuses anything bigger (it's not a USB stick) unless
# --force.
MAX_STICK_BYTES = 512 * 1024**3
