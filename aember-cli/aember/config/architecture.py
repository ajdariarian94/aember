from enum import Enum


class Architecture(str, Enum):
    arm64_linux = "aarch64-poky-linux"
    x64_linux = "x64-poky-linux"
