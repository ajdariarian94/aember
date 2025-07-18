
import getpass
import os
import subprocess
import signal
from typing import List

IMAGE_NAME = "aember"
BASE_IMAGE = "ubuntu:24.04"

def run_docker(command: str, no_tty=False, no_gpu=False, env: List[str] = {}):
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