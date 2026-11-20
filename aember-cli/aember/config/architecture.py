from enum import Enum


class Architecture(str, Enum):
    arm64_linux = "aarch64-poky-linux"
    arm_linux = "arm-poky-linux"
    x64_linux = "x64-poky-linux"
    riscv64_linux = "riscv64-poky-linux"
    x64_linux_musl = "x64-poky-linux-musl"
    arm64_linux_musl = "aarch64-poky-linux-musl"
    arm_linux_musl = "arm-poky-linux-musl"
    riscv64_linux_musl = "riscv64-poky-linux-musl"
