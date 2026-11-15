from enum import Enum


class BuildType(str, Enum):
    release = "Release"
    debug = "Debug"