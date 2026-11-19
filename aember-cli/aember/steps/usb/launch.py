"""
`aember usb launch`: boot the USB image like a PC boots the stick.

QEMU q35 with UEFI firmware (OVMF), the image as a USB mass storage device
on an xHCI controller (the only boot device), an Intel e1000e NIC and the
i6300esb watchdog: firmware → systemd-boot → kernel → aember, the same
path as on real hardware. `-snapshot`: the image stays exactly what gets
flashed.

The default menu entry puts aember's console on the screen. Headless (no
screen) there is only the serial port, so the launcher picks the
"serial console" entry in systemd-boot's menu through the serial port (the
firmware's console includes it): it waits for the menu, then presses
Down and Enter.
"""

import logging
import os
import subprocess
import sys
import threading
from pathlib import Path

from aember.config import usb
from aember.core.context import Context
from aember.core.step import Step

from .image import image_path

log = logging.getLogger("aember.usb.launch")

OVMF = Path("/usr/share/ovmf/OVMF.fd")
DOWN = b"\x1b[B"
ENTER = b"\r"


def qemu_command(img: Path, ovmf: Path, headless: bool) -> list[str]:
    cmd = [
        "qemu-system-x86_64",
        "-machine",
        "q35",
        "-cpu",
        "max",
        "-m",
        "1G",
        "-smp",
        "2",
        "-bios",
        str(ovmf),
        "-drive",
        f"if=none,id=stick,file={img},format=raw",
        "-snapshot",
        "-device",
        "qemu-xhci,id=xhci",
        "-device",
        "usb-storage,bus=xhci.0,drive=stick,bootindex=0",
        "-nic",
        "user,model=e1000e",
        "-device",
        "i6300esb",
        "-serial",
        "stdio",
        "-monitor",
        "none",
    ]
    # Keep the VGA device headless too: the firmware's GOP is efifb/fbcon.
    cmd += ["-display", "none"] if headless else ["-display", "gtk"]
    return cmd


class MenuPicker:
    """
    Watches the serial output for systemd-boot's menu and returns the keys
    that select the serial-console entry, once.
    """

    def __init__(self, title: str = usb.ENTRIES[1][1]) -> None:
        self._needle = title.encode()
        self._tail = b""
        self.done = False

    def feed(self, chunk: bytes) -> bytes:
        if self.done:
            return b""
        self._tail = (self._tail + chunk)[-4096:]
        if self._needle in self._tail:
            self.done = True
            return DOWN + ENTER
        return b""


class UsbLaunchStep(Step):
    def run(self, ctx: Context) -> None:
        img = image_path(ctx.root)
        if not img.exists():
            raise FileNotFoundError(f"{img} not found: run `aember usb image`")
        if not OVMF.exists():
            raise FileNotFoundError(f"{OVMF} not found (apt install ovmf)")
        headless = bool(ctx.data.get("headless"))
        timeout = ctx.data.get("timeout")
        cmd = qemu_command(img, OVMF, headless)
        log.info(
            "Booting %s as a USB stick (UEFI, %s, timeout=%s s)",
            img.name,
            "headless" if headless else "window",
            timeout,
        )
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        picker = MenuPicker() if headless else None
        lock = threading.Lock()

        def to_qemu(data: bytes) -> None:
            with lock:
                try:
                    proc.stdin.write(data)
                    proc.stdin.flush()
                except (BrokenPipeError, ValueError):
                    pass

        def pump_out() -> None:
            fd = proc.stdout.fileno()
            while chunk := os.read(fd, 4096):
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                if picker and (keys := picker.feed(chunk)):
                    log.debug("Selecting the serial console entry")
                    to_qemu(keys)

        def pump_in() -> None:
            try:
                while data := os.read(sys.stdin.fileno(), 1024):
                    to_qemu(data)
            except OSError:
                pass

        out = threading.Thread(target=pump_out, daemon=True)
        out.start()
        threading.Thread(target=pump_in, daemon=True).start()
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            log.info("QEMU stopped after %s s", timeout)
            proc.kill()
            proc.wait()
        out.join(timeout=5)
