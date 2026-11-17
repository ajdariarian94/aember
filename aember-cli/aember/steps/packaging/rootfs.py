import logging
import math
import os
import subprocess
import sys
from pathlib import Path

from aember.config.host_path import HOST_ROOT_PATH
from aember.core.context import Context
from aember.steps.packaging.docker import get_running_container_image

log = logging.getLogger("aember.pack.rootfs")

# ext4 metadata, journal and reserved blocks need room on top of the files.
SIZE_MARGIN = 1.25
SIZE_EXTRA_MB = 64


def _tree_size(path: Path) -> int:
    """Bytes of all regular files under path (symlinks not followed)."""
    if not path.exists():
        return 0
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            f = Path(root) / name
            if not f.is_symlink():
                total += f.stat().st_size
    return total


def rootfs_size_mb(ctx: Context, *trees: Path) -> int:
    """ctx.rootfs_size, or more if the trees wouldn't fit (Debug + tests)."""
    needed = sum(_tree_size(t) for t in trees) / (1024 * 1024)
    return max(ctx.rootfs_size, math.ceil(needed * SIZE_MARGIN) + SIZE_EXTRA_MB)


def create_rootfs_image(
    ctx: Context,
    initramfs_root: Path,
    overlay_dir: Path,
    package_dir: Path,
) -> Path:
    """
    Create a rootfs disk image using guestfish inside Docker.

    - initramfs_root: path to the initramfs contents
    - overlay_dir: additional overlay files to copy in
    - package_dir: directory where rootfs.img will be created
    """
    rootfs_img = package_dir / "aember-rootfs.img"

    # Sparse image, sized for its contents (a Debug build with tests is
    # bigger than the default 512 MB)
    size_mb = rootfs_size_mb(ctx, initramfs_root, overlay_dir)
    with rootfs_img.open("wb") as f:
        f.truncate(size_mb * 1024 * 1024)

    # Build guestfish script
    guestfish_script = [
        f"add {rootfs_img}",
        "run",
        "part-disk /dev/sda mbr",
        "mkfs ext4 /dev/sda1",
        "mount /dev/sda1 /",
    ]

    # Copy initramfs contents
    guestfish_script += [f"copy-in {entry} /" for entry in initramfs_root.iterdir()]

    # Copy overlay if it exists
    if overlay_dir.exists():
        log.info("Applying overlay from %s", overlay_dir)
        guestfish_script += [f"copy-in {entry} /" for entry in overlay_dir.iterdir()]
    else:
        log.debug("No overlay directory at %s, skipping", overlay_dir)

    guestfish_script += ["umount /", "exit"]

    image = get_running_container_image()

    docker_cmd = [
        "docker",
        "run",
        "-i",
        "--rm",
        "--privileged",
        "-u",
        "root",
        "-v",
        f"{HOST_ROOT_PATH}:/aember_ws/aember",
        image,
        "guestfish",
        "--rw",
    ]

    log.info("Building rootfs image (%d MB) at %s", size_mb, rootfs_img)

    # subprocess.run used directly (not CommandRunner) because we need to
    # pipe the guestfish script via stdin, which CommandRunner doesn't support.
    result = subprocess.run(
        docker_cmd,
        input="\n".join(guestfish_script).encode(),
        stderr=subprocess.PIPE,
    )
    stderr = result.stderr.decode(errors="replace")
    sys.stderr.write(stderr)
    # guestfish reading a script from stdin can exit 0 after a failed command
    # (e.g. copy-in hitting "No space left on device"): treat any libguestfs
    # error as a failure, never ship a half-written image.
    if result.returncode != 0 or "libguestfs: error" in stderr:
        raise RuntimeError(
            f"guestfish failed building {rootfs_img} "
            f"(exit {result.returncode}): {stderr.strip()[-500:]}"
        )

    log.info("Rootfs image created successfully: %s", rootfs_img)
    return rootfs_img
