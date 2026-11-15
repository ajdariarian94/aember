from pathlib import Path

from aember.utils.runner import CommandRunner

INITRAMFS_DIRS = [
    "bin",
    "lib",
    "lib64",
    "dev",
    "proc",
    "sys",
    "usr/bin",
    "usr/lib/lxc/rootfs",
    "var/log",
    "tmp",
]


def create_initramfs_tree(root: Path) -> None:
    for d in INITRAMFS_DIRS:
        (root / d).mkdir(parents=True, exist_ok=True)


def pack_initramfs(initramfs_root: Path, package_dir: Path) -> Path:
    output = package_dir / "aember-initramfs.cpio.gz"

    CommandRunner.run(
        f"find . | cpio -H newc -o | gzip -9 > {output}",
        cwd=initramfs_root,
        shell=True,
    )

    return output
