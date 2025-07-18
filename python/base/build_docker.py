
import getpass
import os
import subprocess

from enum import Enum
IMAGE_NAME = "aember"
BASE_IMAGE = "ubuntu:24.04"

def build_docker():
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