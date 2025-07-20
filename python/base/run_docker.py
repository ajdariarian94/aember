import os
import getpass
import subprocess

from python.config import load_config

def run_docker():
    config = load_config()
    docker_path = os.getcwd()
    local_path = os.getcwd()
    username = getpass.getuser()

    # Check if container is already running
    process = subprocess.Popen(
        f"docker ps -q -f name={config["image_image"]}",
        shell=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
    )
    output = process.stdout.read()
    process.wait()

    if not output:
        docker_cmd = (
            f"docker run -it --rm --net host "
            f"--user {config["uid"]}:{config["gid"]} "
            f"--workdir {docker_path} "
            f"-v {local_path}:{docker_path} "
            f"--name {config["image_image"]} "
            f"--privileged "
            f"-e UID={config["uid"]} -e GID={config["gid"]} -e USERNAME={username} "
            f"{config["image_image"]}"
        )
    else:
        docker_cmd = f"docker exec -it -u {config["uid"]}:{config["gid"]} {config["image_image"]}"

    subprocess.run(f"{docker_cmd} {docker_path}/tools/scripts/init.sh", shell=True, check=True)
