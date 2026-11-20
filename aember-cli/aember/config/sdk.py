# Maps build system architecture → SDK environment-setup filename
SDK_ENV_MAP: dict[str, str] = {
    "x64-poky-linux": "environment-setup-core2-64-poky-linux",
    "aarch64-poky-linux": "environment-setup-cortexa53-poky-linux",
    "arm-poky-linux": "environment-setup-armv7at2hf-neon-poky-linux-gnueabi",
    "riscv64-poky-linux": "environment-setup-riscv64imafdc-poky-linux",
    "x64-poky-linux-musl": "environment-setup-core2-64-poky-linux-musl",
    "aarch64-poky-linux-musl": "environment-setup-cortexa53-poky-linux-musl",
}

# Maps build system architecture → target sysroot name inside the SDK
SDK_SYSROOT_MAP: dict[str, str] = {
    "x64-poky-linux": "core2-64-poky-linux",
    "aarch64-poky-linux": "cortexa53-poky-linux",
    "arm-poky-linux": "armv7at2hf-neon-poky-linux-gnueabi",
    "riscv64-poky-linux": "riscv64imafdc-poky-linux",
    "x64-poky-linux-musl": "core2-64-poky-linux-musl",
    "aarch64-poky-linux-musl": "cortexa53-poky-linux-musl",
}

# Maps build system architecture → vcpkg triplet
VCPKG_TRIPLET_MAP: dict[str, str] = {
    "x64-poky-linux": "x64-poky-linux",
    "aarch64-poky-linux": "aarch64-poky-linux",
    "arm-poky-linux": "arm-poky-linux",
    "riscv64-poky-linux": "riscv64-poky-linux",
    "x64-poky-linux-musl": "x64-poky-linux-musl",
    "aarch64-poky-linux-musl": "aarch64-poky-linux-musl",
}
