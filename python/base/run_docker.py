import os
import getpass
import subprocess

IMAGE_NAME = "aember"
UID = 1000
GID = 1000
USERNAME = getpass.getuser()  # or whatever username you want

def run_docker():
    docker_path = os.getcwd()
    local_path = os.getcwd()

    # Check if container is already running
    process = subprocess.Popen(
        f"docker ps -q -f name={IMAGE_NAME}",
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
            f"--user {UID}:{GID} "
            f"--workdir {docker_path} "
            f"-v {local_path}:{docker_path} "
            f"--name {IMAGE_NAME} "
            f"--privileged "
            f"-e UID={UID} -e GID={GID} -e USERNAME={USERNAME} "
            f"{IMAGE_NAME}"
        )
    else:
        docker_cmd = f"docker exec -it -u {UID}:{GID} {IMAGE_NAME}"

    subprocess.run(f"{docker_cmd} {docker_path}/python/scripts/init", shell=True, check=True)
