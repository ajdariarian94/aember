import shutil
from pathlib import Path

from aember.config.qemu import QEMU_LD_MAP, QEMU_RUNNER_MAP
from aember.config.resources import busybox_dir
from aember.utils.runner import CommandRunner


def install_busybox(architecture: str, root: Path, initramfs_root: Path) -> None:
    busybox_src = busybox_dir(root, architecture) / "busybox"
    busybox_dst = initramfs_root / "bin" / "busybox"

    if not busybox_src.exists():
        raise FileNotFoundError(
            f"BusyBox binary not found at {busybox_src}. "
            f"Run `aember qemu build` to fetch it from aember-resources."
        )

    busybox_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(busybox_src, busybox_dst)
    busybox_dst.chmod(0o755)

    bin_dir = initramfs_root / "bin"

    # Build runner command — prepend qemu binary for cross architectures.
    qemu_prefix = QEMU_RUNNER_MAP.get(architecture, [])
    runner = [*qemu_prefix, str(busybox_dst)]

    env = None
    if ld_path := QEMU_LD_MAP.get(architecture):
        env = {"LD_LIBRARY_PATH": ld_path}

    CommandRunner.run(
        [*runner, "--install", "-s", str(bin_dir)],
        env=env,
    )

    # Normalize all symlinks to point at /bin/busybox.
    for entry in bin_dir.iterdir():
        if entry.is_symlink():
            entry.unlink()
            entry.symlink_to("/bin/busybox")
