"""
The containers a board's containers.json lists, as packages in its overlay.

containers.json says only which containers start and what each depends on:

    {"allow_unsigned": true,
     "bridges": [{"name": "lxcbr0", "address": "10.0.3.1/24"}],
     "containers": [{"name": "aember-stats"},
                    {"name": "aember-monitor", "dependencies": ["aember-stats"]}]}

Everything else about a container is in its package,
resources/<arch>/rootfs/var/lib/aember/packages/<name>.aember (git-ignored:
from aember's release, or `aember-cli/run.py install` in the container's
repository; doc/aember-format.md). Only the listed packages go into a
board's image, installed already (steps/packaging/overlay.py), and only
they are needed to build one (utils/check_resources.py).
"""

import json
from pathlib import Path

CONTAINERS_JSON = Path("etc/aember/containers.json")
# Where packages wait to be installed, in an overlay and on a device.
PACKAGES_DIR = Path("var/lib/aember/packages")
# Where they are installed: var/lib/aember/<name>/.
INSTALL_DIR = Path("var/lib/aember")
# Public keys packages may be signed with.
KEYS_DIR = Path("etc/aember/keys")


def read_containers_json(overlay: Path) -> dict:
    """The overlay's containers.json ({} if there is none). Raises
    ValueError when it can't be parsed or is in the format before packages:
    packing would otherwise silently leave out every container."""
    path = overlay / CONTAINERS_JSON
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text())
        entries = data.get("containers", [])
        if not isinstance(entries, list):
            raise TypeError("'containers' is not a list")
    except (OSError, ValueError, AttributeError, TypeError) as e:
        raise ValueError(f"cannot read {path}: {e}") from e
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ValueError(f"{path}: an entry without a name: {entry}")
        if "container" in entry:
            raise ValueError(
                f"{path}: '{entry['name']}' is in the format before .aember "
                "packages: list only names and dependencies "
                "(doc/aember-format.md)"
            )
    return data


def container_names(overlay: Path) -> list[str]:
    """The containers containers.json lists, in its order."""
    return [e["name"] for e in read_containers_json(overlay).get("containers", [])]


def package_path(overlay: Path, name: str) -> Path:
    return overlay / PACKAGES_DIR / f"{name}.aember"


def container_files(overlay: Path) -> list[Path]:
    """The package of every container containers.json lists."""
    return [package_path(overlay, n) for n in container_names(overlay)]


def missing_images(overlay: Path) -> list[Path]:
    """The listed packages the overlay doesn't have."""
    return [f for f in container_files(overlay) if not f.exists()]
