#!/usr/bin/env python3

import argparse
from enum import Enum

from aember import build, clean, configure, execute, pack

class Commands(str, Enum):
    build = "build"
    clean = "clean"
    configure = "configure"
    execute = "execute"
    pack = "pack"
    
def main():
    parser = argparse.ArgumentParser(description="Aember helper script")

    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser(
        f"{Commands.build.value}",
        description="Build CMake project"
    )

    subparsers.add_parser(
        f"{Commands.clean.value}",
        description="Removes everything in build folder except vcpkg_installed"
    )

    subparsers.add_parser(
        f"{Commands.configure.value}", description="Configure the package"
    )

    subparsers.add_parser(
        f"{Commands.execute.value}", description="Execute a configuration"
    )

    subparsers.add_parser(
        f"{Commands.pack.value}",
        description="Pack resources into a bootable container"
    )

    [args, _] = parser.parse_known_args()

    if args.command == Commands.build:
        build()
    elif args.command == Commands.clean:
        clean()
    elif args.command == Commands.configure:
        configure()
    elif args.command == Commands.execute:
        execute()
    elif args.command == Commands.pack:
        pack()
   
if __name__ == "__main__":
    main()
