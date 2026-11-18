"""
`aember rpi launch`: smoke-test the Raspberry Pi SD image in QEMU.

QEMU 8.2 has no Raspberry Pi 4 machine, so this boots QEMU's raspi3b
(BCM2837) with the same kernel, initramfs and SD image and the Pi 3 device
tree from the same kernel build: it exercises the Raspberry Pi boot path
(device tree, SD card driver, root by label on the card, serial console)
without the GPU firmware step, which only the real board runs.
"""

import logging
import subprocess
from pathlib import Path

from aember.config import rpi
from aember.core.context import Context
from aember.core.step import Step

from .image import image_path

log = logging.getLogger("aember.rpi.launch")


class RpiLaunchStep(Step):
    def run(self, ctx: Context) -> None:
        img = image_path(ctx.root)
        boot = ctx.root / "build" / "rpi" / "boot"
        dtb = boot / rpi.DTB_PI3
        for needed in (img, boot / "kernel8.img", dtb):
            if not Path(needed).exists():
                raise FileNotFoundError(f"{needed} not found: run `aember rpi image`")

        cmd = [
            "qemu-system-aarch64",
            "-M",
            "raspi3b",
            "-m",
            "1G",
            "-smp",
            "4",
            "-kernel",
            str(boot / "kernel8.img"),
            "-dtb",
            str(dtb),
            "-initrd",
            str(boot / "aember-initramfs.cpio.gz"),
            # -snapshot: writes go to a temporary overlay, the image stays
            # exactly what gets flashed.
            "-drive",
            f"file={img},if=sd,format=raw",
            "-snapshot",
            "-append",
            rpi.QEMU_PI3_APPEND,
            "-serial",
            "stdio",
            "-display",
            "none",
            "-monitor",
            "none",
        ]
        timeout = ctx.data.get("timeout")
        log.info("Booting the SD image in QEMU raspi3b (timeout=%s s)", timeout)
        try:
            subprocess.run(cmd, timeout=timeout)
        except subprocess.TimeoutExpired:
            log.info("QEMU stopped after %s s", timeout)
