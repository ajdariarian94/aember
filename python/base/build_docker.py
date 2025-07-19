import getpass
import os
import subprocess

IMAGE_NAME = "aember"
BASE_IMAGE = "ubuntu:24.04"

def build_docker():
    docker_cmd = (
        f"docker build --rm -t {IMAGE_NAME} "
        f"--build-arg BASE_IMAGE={BASE_IMAGE} "
        f"--build-arg USERNAME={getpass.getuser()} "
        f"--network=host ."
    )
    subprocess.run(docker_cmd, shell=True, check=True)

