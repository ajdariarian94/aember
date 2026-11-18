"""
Raspberry Pi 4 SD card image: firmware, boot files, layout.

The Pi's GPU firmware (start4.elf + fixup4.dat) reads config.txt from the
FAT boot partition, loads the kernel, the initramfs and the device tree, and
starts the kernel with cmdline.txt. Root is the ext4 second partition, found
by its label (aember has no udev, RootManager reads the superblocks).
"""

ARCHITECTURE = "aarch64-poky-linux"

# raspberrypi/firmware release; files from its boot/ dir, verified by sha256.
FIRMWARE_TAG = "1.20260915"
FIRMWARE_URL = (
    "https://raw.githubusercontent.com/raspberrypi/firmware/{tag}/boot/{name}"
)
FIRMWARE_FILES: dict[str, str] = {
    "start4.elf": "560ead37044d849325755d3684ac1f74654b3ce6450604a42df6dd832cda9ede",
    "fixup4.dat": "d181d5055daebac22513d6904fa9ce21d1e99b852cc391af095ade6f0b45c400",
    "LICENCE.broadcom": "c7283ff51f863d93a275c66e3b4cb08021a5dd4d8c1e7acc47d872fbe52d3d6b",  # noqa: E501
}

# Device trees from the aember-resources kernel asset (kernel/dtb/).
DTB_PI4 = "bcm2711-rpi-4-b.dtb"
DTB_PI3 = "bcm2837-rpi-3-b.dtb"  # QEMU raspi3b smoke test only

ROOT_LABEL = "aember-root"
BOOT_LABEL = "AEMBERBOOT"

# MBR layout in 512-byte sectors: boot (FAT32, type 0x0c) at 4 MiB, 256 MiB.
BOOT_START = 8192
BOOT_SECTORS = 256 * 2048

CONFIG_TXT = """\
# aember on a Raspberry Pi 4 (written by `aember rpi image`)
arm_64bit=1
kernel=kernel8.img
initramfs aember-initramfs.cpio.gz followkernel
device_tree={dtb}
# Serial console on GPIO 14 (TX) / 15 (RX): mini UART, 115200 8N1.
# enable_uart also fixes the core clock the mini UART depends on.
enable_uart=1
disable_overscan=1
"""

# One line. The last console= is /dev/console (aember's console and debug
# shell): the GPIO serial port. Add aember.shell=1 for a shell at boot.
CMDLINE_TXT = (
    "console=tty1 console=ttyS1,115200 "
    f"root=LABEL={ROOT_LABEL} rootfstype=ext4 rootwait rw\n"
)

# Smoke test in QEMU (the Pi 3 machine: QEMU 8.2 has no raspi4b): same
# kernel, initramfs and SD image; its first serial port is the PL011.
# aember.watchdog=0: QEMU's raspi machines reset the moment the BCM2835
# watchdog is started (no countdown emulated), so PID 1 must not arm it
# there; a real Pi counts down and gets the watchdog.
QEMU_PI3_APPEND = (
    f"console=ttyAMA0,115200 root=LABEL={ROOT_LABEL} rootfstype=ext4 rootwait rw "
    "aember.watchdog=0"
)

# `aember rpi flash` refuses anything bigger (it's not an SD card) unless
# --force.
MAX_CARD_BYTES = 256 * 1024**3
