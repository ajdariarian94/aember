import logging
import math
import os
import shutil
import subprocess
import sys
import tarfile
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


def run_guestfish(script: list[str], what: str) -> None:
    """
    Run a guestfish script as root in a `docker run` of the dev image (the
    repo bind-mounted at the same path). Raises on any libguestfs error.
    """
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
        get_running_container_image(),
        "guestfish",
        "--rw",
    ]
    # subprocess.run used directly (not CommandRunner): the script goes in
    # through stdin.
    result = subprocess.run(
        docker_cmd,
        input="\n".join(script).encode(),
        stderr=subprocess.PIPE,
    )
    stderr = result.stderr.decode(errors="replace")
    sys.stderr.write(stderr)
    # guestfish reading a script from stdin can exit 0 after a failed command
    # (e.g. copy-in hitting "No space left on device"): treat any libguestfs
    # error as a failure, never ship a half-written image.
    if result.returncode != 0 or "libguestfs: error" in stderr:
        raise RuntimeError(
            f"guestfish failed building {what} "
            f"(exit {result.returncode}): {stderr.strip()[-500:]}"
        )


def _root_owned(info: tarfile.TarInfo) -> tarfile.TarInfo:
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    return info


def tar_in_commands(trees: list[Path], workdir: Path) -> list[str]:
    """
    guestfish commands that copy each tree's contents onto / of the mounted
    file system, in order (a later tree wins), every file owned by root.

    `copy-in` keeps the build user's uid/gid (1000), which breaks anything
    that checks ownership on the device (an SSH server refuses a /root it
    doesn't own). Each tree goes in as a tar whose entries are uid/gid 0
    instead. The tars are written to workdir; delete it after guestfish ran.
    """
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    commands = []
    for i, tree in enumerate(t for t in trees if t.exists()):
        tar_path = workdir / f"{i:02d}-{tree.name}.tar"
        with tarfile.open(tar_path, "w") as tar:
            for entry in sorted(tree.iterdir()):
                tar.add(entry, arcname=entry.name, filter=_root_owned)
        commands.append(f"tar-in {tar_path} /")
    return commands


def rootfs_size_mb(ctx: Context, *trees: Path) -> int:
    """ctx.rootfs_size, or more if the trees wouldn't fit (Debug + tests)."""
    needed = sum(_tree_size(t) for t in trees) / (1024 * 1024)
    return max(ctx.rootfs_size, math.ceil(needed * SIZE_MARGIN) + SIZE_EXTRA_MB)


def create_rootfs_image(
    ctx: Context,
    initramfs_root: Path,
    overlay_dir: Path,
    package_dir: Path,
    extra_trees: list[Path] | None = None,
) -> Path:
    """
    Create a rootfs disk image using guestfish inside Docker.

    - initramfs_root: path to the initramfs contents
    - overlay_dir: additional overlay files to copy in
    - package_dir: directory where rootfs.img will be created
    - extra_trees: more trees copied in after the initramfs (e.g. tests)
    """
    rootfs_img = package_dir / "aember-rootfs.img"

    # Sparse image, sized for its contents (a Debug build with tests is
    # bigger than the default 512 MB)
    extra = [t for t in (extra_trees or []) if t.exists()]
    size_mb = rootfs_size_mb(ctx, initramfs_root, overlay_dir, *extra)
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

    # initramfs tree, the extra trees, then the overlay (a board's own
    # files win), all owned by root.
    if overlay_dir.exists():
        log.info("Applying overlay from %s", overlay_dir)
    tars = package_dir / "rootfs-tars"
    guestfish_script += tar_in_commands([initramfs_root, *extra, overlay_dir], tars)

    guestfish_script += ["umount /", "exit"]

    log.info("Building rootfs image (%d MB) at %s", size_mb, rootfs_img)
    try:
        run_guestfish(guestfish_script, what=str(rootfs_img))
    finally:
        shutil.rmtree(tars, ignore_errors=True)

    log.info("Rootfs image created successfully: %s", rootfs_img)
    return rootfs_img
