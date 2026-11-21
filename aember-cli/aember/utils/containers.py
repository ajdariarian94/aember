"""
The containers a board's containers.json lists, as files in its overlay.

resources/<arch>/rootfs/var/lib/aember/<name>/ holds a container's image
(<name>.squashfs, git-ignored, from aember's release) and its
lxc.container.conf (tracked). Only the containers containers.json lists go
into a board's image (steps/packaging/overlay.py), and only their images are
needed to build one (utils/check_resources.py).
"""

import json
from pathlib import Path

CONTAINERS_JSON = Path("etc/aember/containers.json")
# Where the overlay keeps the container images: var/lib/aember/<name>/.
IMAGES_DIR = Path("var/lib/aember")


def container_files(overlay: Path) -> list[Path]:
    """
    The squashfs image and LXC config of every container containers.json
    lists, as paths under overlay. No containers.json: none. Raises
    ValueError when it can't be parsed: packing would otherwise silently
    leave out every container.
    """
    path = overlay / CONTAINERS_JSON
    if not path.exists():
        return []
    try:
        entries = json.loads(path.read_text())["containers"]
        if not isinstance(entries, list):
            raise TypeError("'containers' is not a list")
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise ValueError(f"cannot read {path}: {e}") from e

    files: list[Path] = []
    for entry in entries:
        spec = entry.get("container") if isinstance(entry, dict) else None
        if not isinstance(spec, dict):
            continue
        for key in ("squashfs", "config_path"):
            value = spec.get(key)
            if isinstance(value, str) and value:
                files.append(overlay / value.lstrip("/"))
    return files


def missing_images(overlay: Path) -> list[Path]:
    """The listed squashfs images the overlay doesn't have."""
    return [
        f
        for f in container_files(overlay)
        if f.suffix == ".squashfs" and not f.exists()
    ]
