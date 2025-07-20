#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
from enum import Enum

from build import configure, build

IMAGE_NAME = "aember"
BASE_IMAGE = "ubuntu:24.04"


class Commands(str, Enum):
    build = "build"
    build_docker = "build_docker"
    clean = "clean"
    run = "run"
    configure = "configure"
    pack = "pack"
    run_aember = "run_aember"
    debug_aember = "debug_aember"

def main():
    parser = argparse.ArgumentParser(description="CybeeAI helper script")

    parser.add_argument(
        "--no_tty",
        action="store_true",
        help="Run Docker container in non interactive mode",
    )

    parser.add_argument(
        "--no_gpu",
        action="store_true",
        help="Run Docker container in non interactive mode",
    )

    parser.add_argument(
        "--env",
        metavar="key=value",
        action="append",
        help="Add NAME=VALUE environment variables",
    )

    parser.add_argument(
        "--token",
        type=str,
        required=False,
        help="GitHub token for accessing vcpkg packages",
    )

    parser.add_argument(
        "--write",
        required=False,
        action="store_true",
        help="GitHub token permissions for writing vcpkg packages",
    )

    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser(
        f"{Commands.build.value}",
        description="Build CMake project"
    )

    subparsers.add_parser(
        f"{Commands.build_docker.value}",
        description="Build Docker image"
    )

    subparsers.add_parser(
        f"{Commands.configure.value}", description="Configure the package"
    )

    subparsers.add_parser(
        f"{Commands.run.value}", description="Run commands in Docker image"
    )

    subparsers.add_parser(
        f"{Commands.clean.value}",
        description="Removes everything in build folder except vcpkg_installed"
    )

    subparsers.add_parser(
        f"{Commands.pack.value}",
        description="Pack artifacts"
    )

    #subparsers.add_parser(
    #    f"{Commands.run_aember.value}",
    #    description="Run Aember"
    #)

    #subparsers.add_parser(
    #    f"{Commands.debug_aember.value}",
    #    description="Debug Aember"
    #)

    [args, _] = parser.parse_known_args()


    if args.command == Commands.configure:
        configure()
    elif args.command == Commands.build:
        build()
    elif args.command == Commands.pack:
        build_dir = f"{os.getcwd()}/build"

        subprocess.run(["mkdir", "-p", f"{build_dir}/package/initramfs"])
        subprocess.run(["cp", f"{os.getcwd()}/resources/kernel/vmlinuz", f"{build_dir}/package"])
        #subprocess.run(["cp", f"{os.getcwd()}/resources/block_device/block_device.img", f"{build_dir}/package"])
        subprocess.run(["cp", "-a", os.path.join(f"{os.getcwd()}/resources/initramfs"), f"{build_dir}/package"])

        subprocess.run(["cp", f"{build_dir}/install/bin/aember", f"{build_dir}/package/initramfs/usr/bin/aember"])

        #subprocess.run(["sudo mknod -m 666 null c 1 3"], cwd=f"{build_dir}/package/initramfs/dev", shell=True, check=True)
        #subprocess.run(["sudo mknod -m 600 console c 5 1"], cwd=f"{build_dir}/package/initramfs/dev", shell=True, check=True)

        subprocess.run(["find . | cpio -H newc -o > ../initramfs.img"], cwd=f"{build_dir}/package/initramfs", shell=True, check=True)
        subprocess.run(["gzip -9 < ../initramfs.img > ../initramfs.img.gz"], cwd=f"{build_dir}/package/initramfs", shell=True, check=True)
    elif args.command == Commands.run_aember:
        build_dir = f"{os.getcwd()}/build"

        subprocess.run([
            "qemu-system-x86_64",
            "-kernel", "build/package/vmlinuz",                          # path to your kernel
            "-initrd", "build/package/initramfs.img.gz",                # path to your initramfs
            "-nographic",                                               # disable graphical output
            "-append", "console=ttyS0",                    # send boot+shell to serial and VGA
            "-m", "512M",                                               # optional: give QEMU 512MB RAM
            "-cpu", "max",                                              # optional: enable all CPU features
        ], check=True)

        #subprocess.run([
        #    "qemu-system-x86_64",
        #    "-kernel", "build/package/vmlinuz",                          # path to your kernel
        #    "-initrd", "build/package/initramfs.img.gz",                # path to your initramfs
        #    "-nographic",                                               # disable graphical output
        #    "-append", "console=ttyS0 console=tty0",                    # send boot+shell to serial and VGA
        #    "-m", "512M",                                               # optional: give QEMU 512MB RAM
        #    "-cpu", "max",                                              # optional: enable all CPU features
        #], check=True)







        

if __name__ == "__main__":
    main()
