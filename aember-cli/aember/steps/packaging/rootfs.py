import logging
import subprocess
from pathlib import Path

from aember.config.host_path import HOST_ROOT_PATH
from aember.core.context import Context
from aember.steps.packaging.docker import get_running_container_image

log = logging.getLogger("aember.pack.rootfs")


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

    # Create empty image file
    rootfs_img.write_bytes(b"\0" * ctx.rootfs_size * 1024 * 1024)

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

    log.info("Building rootfs image (%d MB) at %s", ctx.rootfs_size, rootfs_img)

    # subprocess.run used directly (not CommandRunner) because we need to
    # pipe the guestfish script via stdin, which CommandRunner doesn't support.
    subprocess.run(
        docker_cmd,
        input="\n".join(guestfish_script).encode(),
        check=True,
    )

    log.info("Rootfs image created successfully: %s", rootfs_img)
    return rootfs_img
