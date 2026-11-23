"""
`aember appimage`: build/appimage/aember-x86_64-v<V>.AppImage from the packed
x64 system (`aember app build` + `aember app pack` first).

    AppDir/
      AppRun                    launcher (files/AppRun): window, root, sandbox
      libexec/aember-sandbox    root helper (files/aember-sandbox): namespaces
      usr/bin/xterm, usr/lib/   the window
      usr/bin/slirp4netns       user-mode NAT
      rootfs/                   the aember system (initramfs tree + services
                                + overlay, as in aember-rootfs.img)
      aember.desktop, aember.png, .DirIcon

The AppImage is the pinned uruntime followed by the AppDir as SquashFS.
"""

import logging
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

from aember.config import appimage as cfg
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.releasing.get_version_from_cmake import get_version_from_cmake
from aember.utils.download import fetch_pinned

log = logging.getLogger("aember.appimage")

FILES = Path(__file__).parent / "files"

DESKTOP = """\
[Desktop Entry]
Type=Application
Name=aember
Comment=Run the aember init system isolated on this machine
Exec=AppRun
Icon=aember
Terminal=false
Categories=Development;System;
"""


def bundled_libs(binary: str) -> list[Path]:
    """The shared libraries @p binary needs, minus the host's own (glibc)."""
    out = subprocess.run(
        ["ldd", binary], capture_output=True, text=True, check=True
    ).stdout
    libs = []
    for line in out.splitlines():
        parts = line.split("=>")
        name = parts[0].strip().split()[0]
        if any(name.startswith(h) for h in cfg.HOST_LIBS):
            continue
        if len(parts) == 2 and parts[1].strip().startswith("/"):
            libs.append(Path(parts[1].strip().split()[0]))
    return libs


def icon_png(size: int = 256) -> bytes:
    """An ember-coloured disc on transparent ground (no image library)."""
    rows = []
    c = (size - 1) / 2
    for y in range(size):
        row = bytearray([0])  # filter: none
        for x in range(size):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5 / c
            if d <= 0.92:
                glow = max(0.0, 1 - d)
                row += bytes((217 + int(38 * glow), 72 + int(110 * glow), 15, 255))
            else:
                row += bytes((0, 0, 0, 0))
        rows.append(bytes(row))

    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
        + chunk(b"IEND", b"")
    )


def appimage_path(root: Path) -> Path:
    version = get_version_from_cmake(str(root / "CMakeLists.txt"))
    return root / "build" / "appimage" / f"aember-x86_64-v{version}.AppImage"


class AppImageStep(Step):
    def run(self, ctx: Context) -> None:
        root = ctx.root
        package = root / "build" / "package" / cfg.ARCHITECTURE / ctx.build_type
        trees = [
            package / "initramfs_root",
            package / "services_root",
            package / "packages_root",
            package / "overlay_root",  # only the containers containers.json lists
        ]
        if not (trees[0] / "usr" / "bin" / "aember").exists() or not trees[3].exists():
            raise FileNotFoundError(
                f"{trees[0]} has no aember: run `aember app build` and "
                "`aember app pack` (x64) first."
            )

        work = root / "build" / "appimage"
        appdir = work / "AppDir"
        shutil.rmtree(appdir, ignore_errors=True)
        (appdir / "rootfs").mkdir(parents=True)
        (appdir / "libexec").mkdir()
        (appdir / "usr" / "bin").mkdir(parents=True)
        (appdir / "usr" / "lib").mkdir()

        # The aember system, as the rootfs image has it.
        for tree in trees:
            if tree.exists():
                subprocess.run(
                    ["cp", "-a", f"{tree}/.", str(appdir / "rootfs")], check=True
                )

        shutil.copy2(FILES / "AppRun", appdir / "AppRun")
        shutil.copy2(FILES / "aember-sandbox", appdir / "libexec" / "aember-sandbox")
        for script in (appdir / "AppRun", appdir / "libexec" / "aember-sandbox"):
            script.chmod(0o755)

        downloads = root / "build" / "downloads" / "appimage"
        slirp = fetch_pinned(cfg.SLIRP4NETNS_URL, cfg.SLIRP4NETNS_SHA256, downloads)
        shutil.copy2(slirp, appdir / "usr" / "bin" / "slirp4netns")
        (appdir / "usr" / "bin" / "slirp4netns").chmod(0o755)

        shutil.copy2(cfg.TERMINAL, appdir / "usr" / "bin" / "xterm")
        for lib in bundled_libs(cfg.TERMINAL):
            shutil.copy2(lib, appdir / "usr" / "lib" / lib.name)
        libs = len(list((appdir / "usr" / "lib").iterdir()))
        log.info("Window: xterm + %d libraries", libs)

        (appdir / "aember.desktop").write_text(DESKTOP)
        (appdir / "aember.png").write_bytes(icon_png())
        (appdir / ".DirIcon").symlink_to("aember.png")

        payload = work / "payload.squashfs"
        payload.unlink(missing_ok=True)
        subprocess.run(
            [
                "mksquashfs",
                str(appdir),
                str(payload),
                "-comp",
                cfg.SQUASHFS_COMP,
                "-all-root",
                "-noappend",
                "-quiet",
            ],
            check=True,
        )

        runtime = fetch_pinned(cfg.RUNTIME_URL, cfg.RUNTIME_SHA256, downloads)
        out = appimage_path(root)
        for old in work.glob("aember-x86_64-v*.AppImage"):
            old.unlink()  # one at a time: they are big
        with out.open("wb") as f:
            for part in (runtime, payload):
                with part.open("rb") as src:
                    shutil.copyfileobj(src, f)
        out.chmod(0o755)
        payload.unlink()
        shutil.rmtree(appdir)  # the AppImage has it all

        log.info("AppImage: %s (%.0f MB)", out, out.stat().st_size / 1e6)
        ctx.data["appimage"] = out
