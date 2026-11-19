"""
`aember rpi flash --device /dev/sdX`: write the SD image to a card.

On WSL a USB card reader is only visible after `usbipd attach --wsl` (from
Windows, usbipd-win). The dev container is privileged and sees the host's
block devices, including WSL's own virtual disks: every write is checked
first and refused unless the device is removable, not a virtual disk, not
larger than an SD card, big enough for the image and not mounted. The user
confirms the exact device before anything is written.
"""

import logging
import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from aember.config import rpi

log = logging.getLogger("aember.rpi.flash")

_DEVICE = re.compile(r"^/dev/(sd[a-z]+|mmcblk[0-9]+)$")


@dataclass
class BlockDevice:
    path: str
    name: str
    size: int
    removable: bool
    model: str


def describe(device: str, sys_block: Path = Path("/sys/block")) -> BlockDevice:
    m = _DEVICE.match(device)
    if not m:
        raise ValueError(f"{device}: give a whole disk like /dev/sdX or /dev/mmcblkN")
    name = m.group(1)
    base = sys_block / name
    if not base.exists():
        raise FileNotFoundError(f"{device}: no such disk (on WSL: usbipd attach?)")

    def read(rel: str) -> str:
        p = base / rel
        return p.read_text().strip() if p.exists() else ""

    return BlockDevice(
        path=device,
        name=name,
        size=int(read("size") or 0) * 512,
        removable=read("removable") == "1",
        model=read("device/model") or read("device/name"),
    )


def problems(
    dev: BlockDevice,
    image_size: int,
    mounts: str,
    force: bool,
    max_bytes: int = rpi.MAX_CARD_BYTES,
    kind: str = "an SD card",
) -> list[str]:
    """Why writing to @p dev is refused (empty: OK)."""
    out = []
    if "virtual disk" in dev.model.lower():
        out.append(f"{dev.path} is a virtual disk ({dev.model}), not {kind}")
    if not dev.removable and not force:
        out.append(f"{dev.path} is not removable (some USB readers aren't: --force)")
    if dev.size > max_bytes and not force:
        out.append(f"{dev.path} is {dev.size / 1024**3:.0f} GiB: too big for {kind}")
    if dev.size < image_size:
        out.append(
            f"{dev.path} ({dev.size} B) is smaller than the image ({image_size} B)"
        )
    mounted = [
        line.split()[0]
        for line in mounts.splitlines()
        if line.split() and re.match(rf"^/dev/{dev.name}(p?[0-9]+)?$", line.split()[0])
    ]
    if mounted:
        out.append(f"{dev.path} is mounted ({', '.join(mounted)}): unmount it first")
    return out


RPI_DONE = (
    "Done: put the card into the Raspberry Pi 4. Serial console: "
    "GPIO 14/15, 115200 8N1."
)


def flash(
    image: Path,
    device: str,
    yes: bool,
    force: bool,
    confirm,
    max_bytes: int = rpi.MAX_CARD_BYTES,
    kind: str = "an SD card",
    build_hint: str = "aember rpi image",
    done: str = RPI_DONE,
) -> None:
    if not image.exists():
        raise FileNotFoundError(f"{image} not found: run `{build_hint}` first")
    dev = describe(device)
    errors = problems(
        dev,
        image.stat().st_size,
        Path("/proc/mounts").read_text(),
        force,
        max_bytes=max_bytes,
        kind=kind,
    )
    if errors:
        raise RuntimeError("Refusing to write: " + "; ".join(errors))

    summary = (
        f"Write {image.name} ({image.stat().st_size / 1024**2:.0f} MiB) to "
        f"{dev.path} ({dev.model or 'unknown model'}, {dev.size / 1024**3:.1f} GiB)?"
        f" EVERYTHING on {dev.path} is lost."
    )
    if not yes and not confirm(summary):
        log.info("Not written.")
        return

    log.info("Writing %s to %s", image, dev.path)
    # dd as root (the device is root's); conv=fsync: on the card when done.
    cmd = [
        "dd",
        f"if={image}",
        f"of={dev.path}",
        "bs=4M",
        "conv=fsync",
        "status=progress",
    ]
    if os.geteuid() != 0:
        cmd = ["sudo", *cmd]
    subprocess.run(cmd, check=True)
    subprocess.run(["sync"], check=False)
    log.info(done)
