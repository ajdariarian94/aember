"""
Ready-to-flash disk images for a release (`aember release`, Release builds):

    build/release/images/
      aember-rpi4-v<V>.img.xz       (+ .sha256)  Raspberry Pi 4 SD card
      aember-x64-usb-v<V>.img.xz    (+ .sha256)  x86-64 PC USB stick (UEFI)
      aember-rpi-imager.json        Raspberry Pi Imager repository
      aember-icon.png               its icon

Raspberry Pi Imager lists aember with
`rpi-imager --repo https://github.com/<repo>/releases/latest/download/aember-rpi-imager.json`
(or App Options → Content Repository → Use custom URL): insert the card,
pick aember, write. Imager downloads, decompresses and verifies the image.
balenaEtcher and `dd` (`xz -dc … | sudo dd of=/dev/sdX`) take the .img.xz
as it is.
"""

import datetime as dt
import hashlib
import json
import logging
import lzma
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step
from aember.steps.appimage.build import icon_png
from aember.steps.rpi.image import RpiImageStep
from aember.steps.rpi.image import image_path as rpi_image_path
from aember.steps.usb.image import UsbImageStep
from aember.steps.usb.image import image_path as usb_image_path
from aember.utils.github import get_repo

from .get_version_from_cmake import get_version_from_cmake
from aember.utils.download import sha256_file

log = logging.getLogger("aember.release.images")

IMAGER_JSON = "aember-rpi-imager.json"
IMAGER_ICON = "aember-icon.png"
IMAGER_DEVICES = ["pi4-64bit"]


def images_dir(root: Path) -> Path:
    return root / "build" / "release" / "images"


def compress(img: Path, out_dir: Path) -> dict:
    """@p img → out_dir/<name>.xz (+ .sha256); sizes and hashes of both."""
    out_dir.mkdir(parents=True, exist_ok=True)
    xz = out_dir / (img.name + ".xz")
    raw = hashlib.sha256()
    tmp = xz.with_suffix(".xz.part")
    # Multi-threaded xz is not in lzma; preset 6 is what xz uses by default.
    with img.open("rb") as src, lzma.open(tmp, "wb", preset=6) as dst:
        for chunk in iter(lambda: src.read(1 << 20), b""):
            raw.update(chunk)
            dst.write(chunk)
    tmp.replace(xz)
    info = {
        "extract_size": img.stat().st_size,
        "extract_sha256": raw.hexdigest(),
        "image_download_size": xz.stat().st_size,
        "image_download_sha256": sha256_file(xz),
    }
    (out_dir / (xz.name + ".sha256")).write_text(
        f"{info['image_download_sha256']}  {xz.name}\n"
    )
    log.info("  ✅ %s (%.1f MB)", xz.name, info["image_download_size"] / 2**20)
    return info


def imager_repo(repo: str, version: str, rpi: dict, released: dt.date) -> dict:
    """Raspberry Pi Imager repository JSON with the aember Pi 4 image."""
    name = f"aember-rpi4-v{version}.img.xz"
    return {
        "os_list": [
            {
                "name": f"aember {version}",
                "description": (
                    "A small PID 1 for container appliances: services, LXC "
                    "containers, watchdog. Serial console on GPIO 14/15."
                ),
                "url": f"https://github.com/{repo}/releases/download/v{version}/{name}",
                "icon": f"https://github.com/{repo}/releases/download/v{version}/"
                f"{IMAGER_ICON}",
                "website": f"https://github.com/{repo}",
                "release_date": released.isoformat(),
                "extract_size": rpi["extract_size"],
                "extract_sha256": rpi["extract_sha256"],
                "image_download_size": rpi["image_download_size"],
                "image_download_sha256": rpi["image_download_sha256"],
                "devices": IMAGER_DEVICES,
            }
        ]
    }


class ReleaseImages(Step):
    """Build, compress and describe the disk image of one architecture."""

    def run(self, ctx: Context) -> None:
        if ctx.build_type.lower() != "release":
            return
        root = ctx.root
        version = get_version_from_cmake(str(root / "CMakeLists.txt"))
        out = images_dir(root)
        if ctx.architecture == "aarch64-poky-linux":
            RpiImageStep().run(ctx)
            info = compress(rpi_image_path(root), out)
            repo = imager_repo(get_repo(root), version, info, dt.date.today())
            (out / IMAGER_JSON).write_text(json.dumps(repo, indent=2) + "\n")
            (out / IMAGER_ICON).write_bytes(icon_png(40))
        elif ctx.architecture == "x64-poky-linux":
            UsbImageStep().run(ctx)
            compress(usb_image_path(root), out)


def release_images(root: Path, version: str) -> list[Path]:
    """The image assets of @p version that `aember release` wrote."""
    out = images_dir(root)
    found = sorted(out.glob(f"aember-*-v{version}.img.xz"))
    files = [*found, *(p.with_name(p.name + ".sha256") for p in found)]
    imager = out / IMAGER_JSON
    if imager.exists() and f"v{version}/" in imager.read_text():
        files += [imager, out / IMAGER_ICON]
    return [f for f in files if f.exists()]
