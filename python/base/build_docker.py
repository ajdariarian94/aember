import getpass
import subprocess

from python.config import load_config

def build_docker():
    config = load_config()

    docker_cmd = (
        f"docker build --rm -t {config["image_image"]} "
        f"--build-arg BASE_IMAGE={config["base_image"]} "
        f"--build-arg USERNAME={getpass.getuser()} "
        f"--network=host ."
    )
    subprocess.run(docker_cmd, shell=True, check=True)

