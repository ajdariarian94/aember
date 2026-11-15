# Maps build system architecture → qemu-user-static binary for cross-execution
QEMU_RUNNER_MAP: dict[str, list[str]] = {
    "x64-poky-linux": [],  # native — no qemu needed
    "aarch64-poky-linux": ["qemu-aarch64-static"],
}

# Maps build system architecture → LD_LIBRARY_PATH for qemu user-static
QEMU_LD_MAP: dict[str, str] = {
    "aarch64-poky-linux": "/usr/aarch64-linux-gnu/lib",
}
