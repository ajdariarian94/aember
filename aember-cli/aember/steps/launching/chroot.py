"""
Run the packed system without QEMU: aember as PID 1 in a chroot of the
rootfs image, on the host kernel, with the containers drawing on the host's
X server (WSLg).

A privileged helper container from the dev image (like the guestfish step)
gives aember its own PID, mount, network, UTS and IPC namespaces. Inside it:

- the rootfs image's partition is mounted read-only (`noload`: a QEMU run
  can leave the ext4 journal needing replay, which a read-only mount can't
  do) with a tmpfs overlay on top, so a run never changes the image;
- /proc, /sys, /dev (the helper's, so /dev/console is this terminal), /tmp
  with the X11 socket and /run are prepared before aember starts; aember
  skips mounts that already exist;
- /etc/aember/lxc-extra.conf passes the display into every container
  (ContainerManager applies it), /etc/resolv.conf comes from Docker;
- `exec chroot <root> /usr/bin/aember --root --chroot`: aember is PID 1 and
  loads no kernel modules (they would go into the host kernel).

Foreign architectures run through qemu-user: a binfmt_misc handler with the
F flag is registered on the host kernel for that architecture if none
exists (it lasts until WSL restarts).

Stopping: Ctrl-C or the timeout run `docker stop`, i.e. SIGTERM to PID 1,
which aember handles as a clean shutdown.
"""

import datetime
import logging
import os
import struct
import subprocess
import sys
from pathlib import Path

from aember.config.host_path import HOST_ROOT_PATH
from aember.config.qemu import QEMU_RUNNER_MAP
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.packaging.docker import get_running_container_image

log = logging.getLogger("aember.launch.chroot")

HOST_ARCH = "x64-poky-linux"
# Run on the build host without qemu-user
NATIVE_ARCHS = (HOST_ARCH, "x64-poky-linux-musl")
CONTAINER_ROOT = "/aember_ws/aember"
X11_HOST_DIR = "/tmp/.X11-unix"

# binfmt_misc magic/mask for ELF executables (from QEMU's
# scripts/qemu-binfmt-conf.sh), \\x-escaped as the register file expects.
_BINFMT = {
    "aarch64-poky-linux": (
        r"\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00",
        r"\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff",
    ),
    "arm-poky-linux": (
        r"\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00",
        r"\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff",
    ),
    "riscv64-poky-linux": (
        r"\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xf3\x00",
        r"\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff",
    ),
    "aarch64-poky-linux-musl": (
        r"\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00",
        r"\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff",
    ),
    "arm-poky-linux-musl": (
        r"\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00",
        r"\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff",
    ),
    "riscv64-poky-linux-musl": (
        r"\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xf3\x00",
        r"\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff",
    ),
}

# Every container gets the host's X server (Qt via xcb).
LXC_EXTRA_X11 = """\
# Written by `aember app launch --chroot`: draw on the host's X server.
lxc.mount.entry = /tmp/.X11-unix tmp/.X11-unix none bind,create=dir,optional 0 0
lxc.environment = DISPLAY={display}
lxc.environment = QT_QPA_PLATFORM=xcb
# No GPU in the container: no GLX/EGL, software rendering (as on linuxfb).
lxc.environment = QT_XCB_GL_INTEGRATION=none
lxc.environment = QT_QUICK_BACKEND=software
lxc.environment = LIBGL_ALWAYS_SOFTWARE=1
"""


def first_partition_offset(image: Path) -> int:
    """Byte offset of the first MBR partition (the rootfs)."""
    with image.open("rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        raise RuntimeError(f"{image} has no MBR partition table")
    start_lba = struct.unpack_from("<I", mbr, 446 + 8)[0]
    if start_lba == 0:
        raise RuntimeError(f"{image}: first partition is empty")
    return start_lba * 512


def binfmt_script(architecture: str) -> str:
    """Shell snippet registering qemu-user for a foreign architecture."""
    if architecture in NATIVE_ARCHS:
        return ""
    if architecture not in _BINFMT:
        raise RuntimeError(f"No binfmt magic known for {architecture}")
    magic, mask = _BINFMT[architecture]
    qemu = QEMU_RUNNER_MAP[architecture][0]
    name = f"aember-{qemu.removesuffix('-static')}"
    return f"""
B=/proc/sys/fs/binfmt_misc
[ -f $B/register ] || mount -t binfmt_misc binfmt_misc $B
if [ ! -e $B/{name} ] && ! grep -qs "^interpreter .*{qemu}" $B/qemu-* 2>/dev/null; then
  echo "aember: registering {qemu} for {architecture} binaries (binfmt_misc, F)"
  printf '%s' ':{name}:M::{magic}:{mask}:/usr/bin/{qemu}:F' > $B/register
fi
"""


def squashfs_script(cache_dir: str) -> str:
    """
    Make the container images mountable by the host kernel.

    aember's kernel mounts zstd SquashFS; WSL's has no zstd SquashFS
    (CONFIG_SQUASHFS_ZSTD is off). zstd images are repacked once with xz into
    {cache_dir} (root, so ownership and device nodes survive; reused while
    the source's size and mtime match) and bind-mounted over the original
    inside the chroot. The image itself is never changed.
    """
    return f"""
C={cache_dir}
mkdir -p $C && chmod 777 $C
find $L -name '*.squashfs' | while read -r src; do
  unsquashfs -s "$src" 2>/dev/null | grep -q '^Compression zstd' || continue
  rel=${{src#$L/}}
  out=$C/$(echo "$rel" | tr / _)
  stamp="$(stat -c %s-%Y "$src")"
  if [ ! -f "$out" ] || [ "$(cat "$out.stamp" 2>/dev/null)" != "$stamp" ]; then
    echo "aember: repacking $rel (zstd -> xz) for the host kernel, once"
    rm -rf /aember/x
    unsquashfs -q -no-progress -d /aember/x "$src" >/dev/null
    mksquashfs /aember/x "$out.tmp" -comp xz -noappend -no-progress -quiet
    mv "$out.tmp" "$out"; echo "$stamp" > "$out.stamp"
    chmod 666 "$out" "$out.stamp"; rm -rf /aember/x
  fi
  mount --bind "$out" "$R/$rel"
done
"""


def helper_script(
    image: str, offset: int, architecture: str, display: str, cache_dir: str
) -> str:
    """Runs as root in the helper container and ends as aember (PID 1)."""
    extra = LXC_EXTRA_X11.format(display=display)
    return f"""set -eu
{binfmt_script(architecture)}
R=/aember/root; L=/aember/lower; U=/aember/rw
mkdir -p $R $L $U
mount -o ro,noload,loop,offset={offset} '{image}' $L
mount -t tmpfs tmpfs $U
mkdir -p $U/upper $U/work
mount -t overlay overlay -o lowerdir=$L,upperdir=$U/upper,workdir=$U/work $R

mkdir -p $R/proc $R/sys $R/dev $R/tmp $R/run $R/etc/aember
mount -t proc proc $R/proc
mount --rbind /sys $R/sys
mount --rbind /dev $R/dev
mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs $R/tmp
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs $R/run
cp /etc/resolv.conf $R/etc/resolv.conf
{squashfs_script(cache_dir)}

if [ -S /x11/X0 ] || [ -n "$(ls -A /x11 2>/dev/null)" ]; then
  mkdir -p $R/tmp/.X11-unix
  mount --bind /x11 $R/tmp/.X11-unix
  cat > $R/etc/aember/lxc-extra.conf <<'EOF'
{extra}EOF
  echo "aember: X11 display {display} passed to the containers"
else
  echo "aember: no X11 socket, containers run without a display"
fi

exec chroot $R /usr/bin/aember --root --chroot
"""


class ChrootLaunchStep(Step):
    def run(self, ctx: Context) -> None:
        package_dir = ctx.root / "build" / "package" / ctx.architecture / ctx.build_type
        image = package_dir / "aember-rootfs.img"
        if not image.exists():
            raise FileNotFoundError(f"{image} not found; run `aember app pack` first.")

        if ctx.architecture not in NATIVE_ARCHS:
            log.warning(
                "%s runs through qemu-user: aember, its services and the "
                "network work, LXC containers don't (qemu-user can't create "
                "namespaces). Use the QEMU boot or the board for containers.",
                ctx.architecture,
            )
        offset = first_partition_offset(image)
        image_in_helper = f"{CONTAINER_ROOT}/{image.relative_to(ctx.root)}"
        display = os.environ.get("DISPLAY", ":0")
        name = f"aember-chroot-{ctx.architecture}-{os.getpid()}"

        log_dir = ctx.root / "build" / "logging" / ctx.architecture
        log_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        log_path = log_dir / f"chroot-{ts}.log"

        interactive = sys.stdin.isatty() and sys.stdout.isatty()
        cmd = [
            "docker",
            "run",
            "--rm",
            "--privileged",
            "-u",
            "root",
            "--name",
            name,
            "--hostname",
            "aember",
            "-i",
            *(["-t"] if interactive else []),
            "-v",
            f"{HOST_ROOT_PATH}:{CONTAINER_ROOT}",
            "-v",
            f"{X11_HOST_DIR}:/x11",
            get_running_container_image(),
            "bash",
            "-c",
            helper_script(
                image_in_helper,
                offset,
                ctx.architecture,
                display,
                f"{CONTAINER_ROOT}/build/chroot-cache/{ctx.architecture}",
            ),
        ]

        timeout = ctx.data.get("timeout")
        log.info(
            "Running %s (%s) in a chroot as %s (timeout=%s s, log %s)",
            ctx.architecture,
            ctx.build_type,
            name,
            timeout,
            log_path,
        )
        with log_path.open("wb") as log_file:
            # Interactive: the terminal belongs to aember's console (debug
            # shell with 'd'); otherwise stream and keep a log.
            proc = subprocess.Popen(
                cmd,
                stdout=None if interactive else subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            try:
                if interactive:
                    proc.wait(timeout=timeout)
                else:
                    self._stream(proc, log_file, timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                log.info("Stopping %s (SIGTERM to aember)", name)
                subprocess.run(
                    ["docker", "stop", "-t", "20", name],
                    capture_output=True,
                )
                proc.wait()
        log.info("chroot run ended (exit %s)", proc.returncode)

    @staticmethod
    def _stream(proc: subprocess.Popen, log_file, timeout: int | None) -> None:
        """Copy the console to stdout and the log until exit or timeout."""
        import selectors
        import time

        deadline = time.monotonic() + timeout if timeout else None
        sel = selectors.DefaultSelector()
        sel.register(proc.stdout, selectors.EVENT_READ)
        while True:
            wait = None if deadline is None else deadline - time.monotonic()
            if wait is not None and wait <= 0:
                raise subprocess.TimeoutExpired(proc.args, timeout)
            if not sel.select(wait):
                continue
            chunk = proc.stdout.read1(65536)
            if not chunk:
                proc.wait()
                return
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            log_file.write(chunk)
