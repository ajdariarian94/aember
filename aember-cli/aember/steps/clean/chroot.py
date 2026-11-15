import subprocess
from pathlib import Path


def cleanup_chroot(initramfs_root: Path) -> None:
    """
    Unmount any bind mounts under initramfs_root (host-side),
    then delete the directory tree.
    """
    if not initramfs_root.is_dir():
        return

    # Read /proc/mounts to find ALL mounts under initramfs_root
    try:
        mounts_text = Path("/proc/mounts").read_text()
        mounted = [
            line.split()[1]
            for line in mounts_text.splitlines()
            if line.split()[1].startswith(str(initramfs_root))
        ]

        # Sort in reverse — deepest mounts first
        for mount_point in sorted(mounted, reverse=True):
            subprocess.run(
                ["sudo", "umount", "-f", mount_point],
                check=False,
            )
    except Exception:
        pass

    # Use sudo rm -rf — some files may be owned by root
    subprocess.run(
        ["sudo", "rm", "-rf", str(initramfs_root)],
        check=False,
    )
