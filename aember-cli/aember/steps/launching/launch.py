import datetime
import logging
import re
import subprocess
import threading
from dataclasses import dataclass, replace
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.launch")


# ---------------------------------------------------------------------------
# QEMU architecture profiles
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class QemuProfile:
    binary: str
    machine: str | None
    cpu: str
    drive: str  # if=... format=raw suffix
    console: str  # kernel append console=...
    root: str  # kernel append root=...
    display: tuple[str, ...] = ()  # display args — empty = headless

    def base_cmd(
        self, kernel: str, initrd: str, rootfs: str, kernel_args: str = ""
    ) -> list[str]:
        cmd = [self.binary]
        if self.machine:
            cmd += ["-machine", self.machine]
        cmd += [
            "-cpu",
            self.cpu,
            "-m",
            "512M",
            "-kernel",
            kernel,
            "-initrd",
            initrd,
            "-drive",
            f"file={rootfs},{self.drive}",
            "-append",
            f"{self.console} {self.root} rw rootwait {kernel_args}".rstrip(),
            "-netdev",
            "user,id=net0",
            "-device",
            "virtio-net-pci,netdev=net0",
            # QEMU's PCI watchdog (a real countdown, default action reset):
            # PID 1 feeds it like a board's /dev/watchdog (kernel driver
            # i6300esb from aember-resources v0.1.6).
            "-device",
            "i6300esb",
        ]
        if self.display:
            cmd += list(self.display)
        else:
            cmd += ["-display", "none", "-vga", "none"]
        return cmd


def extra_kernel_args(debug_shell: bool, append: str) -> str:
    """
    What `aember app launch` adds to the kernel command line: aember.shell=1
    (PID 1 opens a debug shell at boot) and the user's --append text.
    """
    args = ["aember.shell=1"] if debug_shell else []
    args += append.split()
    return " ".join(args)


QEMU_PROFILES: dict[str, QemuProfile] = {
    "x64-poky-linux": QemuProfile(
        binary="qemu-system-x86_64",
        machine=None,
        cpu="max",
        drive="if=ide,format=raw",
        console="console=ttyS0",
        root="root=/dev/sda1",
        display=("-vga", "virtio", "-display", "gtk"),
    ),
    "x64-poky-linux-musl": QemuProfile(
        binary="qemu-system-x86_64",
        machine=None,
        cpu="max",
        drive="if=ide,format=raw",
        console="console=ttyS0",
        root="root=/dev/sda1",
        display=("-vga", "virtio", "-display", "gtk"),
    ),
    "aarch64-poky-linux": QemuProfile(
        binary="qemu-system-aarch64",
        machine="virt",
        # The SDK targets generic ARMv8.0-A (Cortex-A53, aember-resources
        # v0.1.8); test on the smallest core it supports.
        cpu="cortex-a53",
        drive="if=virtio,format=raw",
        console="console=tty0 console=ttyAMA0,115200",
        root="root=/dev/vda1",
        display=("-device", "virtio-gpu-pci", "-display", "gtk"),
    ),
    "arm-poky-linux": QemuProfile(
        binary="qemu-system-arm",
        # highmem=off: the kernel has no LPAE, so keep all devices below 4 GB
        machine="virt,highmem=off",
        cpu="cortex-a15",  # ARMv7-A with NEON; the SDK targets generic ARMv7-A
        drive="if=virtio,format=raw",
        console="console=tty0 console=ttyAMA0,115200",
        root="root=/dev/vda1",
        display=("-device", "virtio-gpu-pci", "-display", "gtk"),
    ),
    "riscv64-poky-linux": QemuProfile(
        binary="qemu-system-riscv64",
        # virt loads its built-in OpenSBI firmware (-bios default)
        machine="virt",
        cpu="rv64",  # RV64GC, like the SDK
        drive="if=virtio,format=raw",
        console="console=tty0 console=ttyS0,115200",
        root="root=/dev/vda1",
        display=("-device", "virtio-gpu-pci", "-display", "gtk"),
    ),
    "aarch64-poky-linux-musl": QemuProfile(
        binary="qemu-system-aarch64",
        machine="virt",
        # The SDK targets generic ARMv8.0-A (Cortex-A53, aember-resources
        # v0.1.8); test on the smallest core it supports.
        cpu="cortex-a53",
        drive="if=virtio,format=raw",
        console="console=tty0 console=ttyAMA0,115200",
        root="root=/dev/vda1",
        display=("-device", "virtio-gpu-pci", "-display", "gtk"),
    ),
    "arm-poky-linux-musl": QemuProfile(
        binary="qemu-system-arm",
        # highmem=off: the kernel has no LPAE, so keep all devices below 4 GB
        machine="virt,highmem=off",
        cpu="cortex-a15",  # ARMv7-A with NEON; the SDK targets generic ARMv7-A
        drive="if=virtio,format=raw",
        console="console=tty0 console=ttyAMA0,115200",
        root="root=/dev/vda1",
        display=("-device", "virtio-gpu-pci", "-display", "gtk"),
    ),
    "riscv64-poky-linux-musl": QemuProfile(
        binary="qemu-system-riscv64",
        # virt loads its built-in OpenSBI firmware (-bios default)
        machine="virt",
        cpu="rv64",  # RV64GC, like the SDK
        drive="if=virtio,format=raw",
        console="console=tty0 console=ttyS0,115200",
        root="root=/dev/vda1",
        display=("-device", "virtio-gpu-pci", "-display", "gtk"),
    ),
}

# Maps architecture → GDB binary and extra setup commands
GDB_PROFILES: dict[str, dict] = {
    "x64-poky-linux": {
        "binary": "gdb",
        "setup": [],
    },
    "x64-poky-linux-musl": {
        "binary": "gdb",
        "setup": [],
    },
    "aarch64-poky-linux": {
        "binary": "gdb-multiarch",
        "setup": ["-ex", "set architecture aarch64"],
    },
    "arm-poky-linux": {
        "binary": "gdb-multiarch",
        "setup": ["-ex", "set architecture arm"],
    },
    "riscv64-poky-linux": {
        "binary": "gdb-multiarch",
        "setup": ["-ex", "set architecture riscv:rv64"],
    },
    "aarch64-poky-linux-musl": {
        "binary": "gdb-multiarch",
        "setup": ["-ex", "set architecture aarch64"],
    },
    "arm-poky-linux-musl": {
        "binary": "gdb-multiarch",
        "setup": ["-ex", "set architecture arm"],
    },
    "riscv64-poky-linux-musl": {
        "binary": "gdb-multiarch",
        "setup": ["-ex", "set architecture riscv:rv64"],
    },
}

XTERM_OPTS = ["-geometry", "120x40", "-fa", "Monospace", "-fs", "14"]


# ---------------------------------------------------------------------------
# Serial log cleanup
# ---------------------------------------------------------------------------


def _clean_serial_log(raw_path: str, clean_path: str) -> None:
    """Strip ANSI escape codes and normalize line endings from a raw serial log."""
    try:
        raw = Path(raw_path).read_bytes()
        clean = re.sub(rb"\x1B\[[0-9;?]*[mGKHFlh]", b"", raw)  # CSI sequences
        clean = re.sub(rb"\x1B[cM]", b"", clean)  # ESC c / ESC M
        clean = re.sub(rb"\x1B\][^\x07]*\x07", b"", clean)  # OSC sequences
        clean = re.sub(rb"\r\n", b"\n", clean)
        clean = re.sub(rb"\r", b"\n", clean)
        Path(clean_path).write_bytes(clean)
        Path(raw_path).unlink()
        log.info("Serial log written to %s", clean_path)
    except Exception as e:
        log.warning("Failed to clean serial log: %s", e)


def _cleanup_thread(proc: subprocess.Popen, raw: str, clean: str) -> None:
    threading.Thread(
        target=lambda: (proc.wait(), _clean_serial_log(raw, clean)),
        daemon=True,
    ).start()


def _xterm(title: str, cmd: list[str]) -> subprocess.Popen:
    return subprocess.Popen(
        [
            "xterm",
            *XTERM_OPTS,
            "-hold",
            "-title",
            title,
            "-e",
            *cmd,
        ]
    )


def _gdb_cmd(ctx: Context, aember_path: str) -> list[str]:
    """Build the GDB command for the target architecture."""
    profile = GDB_PROFILES.get(ctx.architecture, GDB_PROFILES["x64-poky-linux"])
    return [
        profile["binary"],
        aember_path,
        *profile["setup"],
        "-ex",
        "target remote localhost:1234",
        "-ex",
        "break main",
        "-ex",
        "continue",
    ]


# ---------------------------------------------------------------------------
# Step
# ---------------------------------------------------------------------------


class LaunchStep(Step):

    def run(self, ctx: Context) -> None:
        log.info("Launching Aember in QEMU")

        profile = QEMU_PROFILES.get(ctx.architecture)
        if not profile:
            raise ValueError(
                f"Unsupported architecture: '{ctx.architecture}'. "
                f"Known: {list(QEMU_PROFILES)}"
            )

        package_dir = ctx.root / "build" / "package" / ctx.architecture / ctx.build_type
        install_dir = ctx.root / "build" / "install" / ctx.architecture / ctx.build_type

        kernel_path = str(package_dir / "kernel")
        initrd_path = str(package_dir / "aember-initramfs.cpio.gz")
        rootfs_img = str(package_dir / "aember-rootfs.img")
        aember_path = str(install_dir / "bin" / "aember")

        log.debug("Kernel:    %s", kernel_path)
        log.debug("Initramfs: %s", initrd_path)
        log.debug("Rootfs:    %s", rootfs_img)
        log.debug("Aember:    %s", aember_path)

        # Serial log
        log_dir = ctx.root / "build" / "logging" / ctx.architecture
        log_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        raw_log_path = str(log_dir / f"qemu-serial-{ts}.log.raw")
        serial_log_path = str(log_dir / f"qemu-serial-{ts}.log")

        kernel_args = extra_kernel_args(
            ctx.data.get("debug_shell", False), ctx.data.get("append", "")
        )
        base_cmd = profile.base_cmd(kernel_path, initrd_path, rootfs_img, kernel_args)
        serial_chardevs = [
            "-chardev",
            f"stdio,id=char0,logfile={raw_log_path},logappend=on",
            "-serial",
            "chardev:char0",
        ]

        log.debug("QEMU base command: %s", " ".join(base_cmd))

        # ---------------------------------------------------------------------------
        # HEADLESS — foreground, no windows, serial console on stdout
        # ---------------------------------------------------------------------------
        if ctx.data.get("headless"):
            timeout = ctx.data.get("timeout")
            # Keep the virtual GPU (containers bind /dev/dri, /dev/fb0), only
            # swap the GTK window for no display.
            headless_display = tuple(
                "none" if arg == "gtk" else arg for arg in profile.display
            )
            cmd = replace(profile, display=headless_display).base_cmd(
                kernel_path, initrd_path, rootfs_img, kernel_args
            )
            cmd += serial_chardevs + ["-monitor", "none"]
            log.info("Launching QEMU headless (timeout=%s s)", timeout)
            try:
                subprocess.run(cmd, timeout=timeout)
            except subprocess.TimeoutExpired:
                log.info("QEMU stopped after %s s", timeout)
            _clean_serial_log(raw_log_path, serial_log_path)
            return

        # ---------------------------------------------------------------------------
        # RELEASE
        # ---------------------------------------------------------------------------
        if ctx.build_type.lower() == "release":
            log.info("Launching QEMU (release mode)")
            qemu_proc = _xterm("Aember QEMU", base_cmd + serial_chardevs)
            _cleanup_thread(qemu_proc, raw_log_path, serial_log_path)
            return

        # ---------------------------------------------------------------------------
        # DEBUG
        # ---------------------------------------------------------------------------
        log.info("Launching QEMU (debug mode) — waiting for GDB connection")
        qemu_proc = _xterm(
            "Aember QEMU",
            base_cmd + serial_chardevs + ["-S", "-s"],
        )
        _cleanup_thread(qemu_proc, raw_log_path, serial_log_path)

        gdb_cmd = _gdb_cmd(ctx, aember_path)
        log.debug("GDB command: %s", " ".join(gdb_cmd))

        subprocess.run(
            [
                "xterm",
                *XTERM_OPTS,
                "-hold",
                "-title",
                "Aember GDB",
                "-e",
                *gdb_cmd,
            ]
        )

        log.info("QEMU + GDB launched successfully")
