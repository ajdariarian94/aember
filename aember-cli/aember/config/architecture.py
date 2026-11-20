from enum import Enum


class Architecture(str, Enum):
    arm64_linux = "aarch64-poky-linux"
    arm_linux = "arm-poky-linux"
    x64_linux = "x64-poky-linux"
    riscv64_linux = "riscv64-poky-linux"
