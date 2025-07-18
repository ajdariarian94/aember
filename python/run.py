import argparse
import getpass
import os
import shutil
import signal
import subprocess
import sys
from typing import List

from enum import Enum

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


def build_docker_image():
    docker_cmd = f"""docker build -t {IMAGE_NAME} \
        --build-arg BASE_IMAGE={BASE_IMAGE} \
        --build-arg UID={os.getuid()} \
        --build-arg GID={os.getgid()} \
        --build-arg USERNAME={getpass.getuser()} \
        --network=host \
    """
    process = subprocess.Popen(
        f"{docker_cmd} .", shell=True, encoding="utf-8", errors="replace"
    )

    process.wait()

def run_docker_container(command: str, no_tty=False, no_gpu=False, env: List[str] = {}):
    docker_path = os.getcwd()
    local_path = os.getcwd()
    no_tty_flag = "t" if not no_tty else ""
    no_gpu_flag = (
        "--runtime=nvidia -e NVIDIA_DRIVER_CAPABILITIES=all" if not no_gpu else ""
    )

    process = subprocess.Popen(
        f"docker ps -q -f name={IMAGE_NAME}",
        shell=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
    )
    output = process.stdout.read()

    process.wait()
    extra = ""
    for i in env:
        extra = f"{extra} -e {i}"

    if not output:
        docker_cmd = f"""docker run -i{no_tty_flag} --rm --net host \
            {no_gpu_flag} \
            -e DBUS_SESSION_BUS_ADDRESS \
            -v /tmp/.X11-unix:/tmp/.X11-unix \
            -v /dev/bus/usb:/dev/bus/usb \
            -e DISPLAY=$DISPLAY \
            --group-add video \
            {extra} \
            --user {os.getuid()}:{os.getgid()} \
            --ipc=host \
            --cap-add=CAP_SYS_PTRACE \
            --ulimit memlock=-1 \
            --ulimit stack=67108864 \
            --workdir {docker_path} \
            -v /run/user/{os.getuid()}/bus:/run/user/{os.getuid()}/bus \
            -v /run/dbus/system_bus_socket:/run/dbus/system_bus_socket \
            -v {local_path}:{docker_path} \
            -v {os.getcwd()}/keypairs:/home/{getpass.getuser()}/.config/.mono/keypairs \
            --privileged \
            --name {IMAGE_NAME} \
            {IMAGE_NAME}"""
    else:
        docker_cmd = f"docker exec -i{no_tty_flag} {IMAGE_NAME}"

    def signal_handler(sig, frame):
        subprocess.run(f"docker kill {IMAGE_NAME}", shell=True)
        process.terminate()
        exit(0)

    signal.signal(signal.SIGINT, signal_handler)

    process = subprocess.Popen(
        f"{docker_cmd} {command}", shell=True, encoding="utf-8", errors="replace"
    )

    process.wait()

    return process.returncode


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

    subparsers.add_parser(
        f"{Commands.run_aember.value}",
        description="Run Aember"
    )

    subparsers.add_parser(
        f"{Commands.debug_aember.value}",
        description="Debug Aember"
    )

    [args, unknown] = parser.parse_known_args()

    if args.env is None:
        args.env = []

    if args.command == Commands.build_docker:
        build_docker_image()
    elif args.command == Commands.run:
        run_docker_container(" ".join(unknown), args.no_tty, args.no_gpu, args.env)
    elif args.command == Commands.configure:
        command = (
            f"cmake "
            f"-DCMAKE_BUILD_TYPE=Release "
            f"-DCMAKE_INSTALL_PREFIX=build/install "
            f"-DCMAKE_TOOLCHAIN_FILE={os.getcwd()}/tools/toolchains/x86_64-gnu.cmake"
            f"/vcpkg/scripts/buildsystems/vcpkg.cmake "
            f"-DVCPKG_OVERLAY_PORTS={os.getcwd()}/vcpkg_overlays "
            f"-DVCPKG_TARGET_TRIPLET=x64-linux "
            f"-DVCPKG_HOST_TRIPLET=x64-linux "
            f"-B build/x86_64-gnu -S ."
            )

        configuration_result = run_docker_container(
            command,
            args.no_tty,
            args.no_gpu,
            args.env,
        )

        if configuration_result != 0:
            print("Configure failed!")
            sys.exit(1)

    elif args.command == Commands.build:
        build_result = run_docker_container(
            "cmake --build build/x86_64-gnu --parallel",
            args.no_tty,
            args.no_gpu,
            args.env,
        )

        if build_result != 0:
            print("Build failed!")
            sys.exit(1)
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
