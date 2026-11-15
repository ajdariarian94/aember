import re
import subprocess
from pathlib import Path

# Docker bind-mounts /etc/hostname etc. from /var/lib/docker/containers/<id>/
_CONTAINER_ID = re.compile(r"/containers/([0-9a-f]{64})/")


def _own_container_id() -> str | None:
    """ID of the container this process runs in, from /proc/self/mountinfo."""
    try:
        match = _CONTAINER_ID.search(Path("/proc/self/mountinfo").read_text())
    except OSError:
        return None
    return match.group(1) if match else None


def get_running_container_image() -> str:
    """
    Image of the dev container this process runs in.

    Looks up our own container ID (works with several dev containers running,
    e.g. aember and aember-resources). Falls back to the first running
    container if the ID can't be determined.
    """
    if container_id := _own_container_id():
        return subprocess.run(
            ["docker", "inspect", "--format", "{{.Config.Image}}", container_id],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()

    result = subprocess.run(
        ["docker", "container", "ls", "--no-trunc", "--format", "{{.ID}} {{.Image}}"],
        capture_output=True,
        text=True,
        check=True,
    )

    lines = result.stdout.strip().splitlines()
    if not lines:
        raise RuntimeError("No running containers found via 'docker container ls'")

    _, image = lines[0].split(maxsplit=1)
    return image
