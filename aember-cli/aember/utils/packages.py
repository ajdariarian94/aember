"""
Opt-in packages from aember-resources (SSH, Wi-Fi, ...).

A board lists the packages it wants in its overlay's
etc/aember/packages.json:

    {"packages": ["dropbear"]}

FetchResources then downloads only those (besides kernel, BusyBox, LXC and
the SDK) into build/aember-resources/<arch>/packages/<name>/, and the pack
step copies their files into the board's root filesystem
(steps/packaging/packages.py). A package nobody lists is neither downloaded
nor packed. Each package is the tree aember-resources' aember-package class
deploys: rootfs/, packages.txt and symlinks.txt.
"""

import json
from pathlib import Path

PACKAGES_JSON = Path("etc/aember/packages.json")


def listed_packages(overlay: Path) -> list[str]:
    """The packages the overlay's packages.json lists, in order, without
    duplicates. No packages.json: none. Raises ValueError when it can't be
    parsed (a board would silently lose its SSH server otherwise)."""
    path = overlay / PACKAGES_JSON
    if not path.exists():
        return []
    try:
        names = json.loads(path.read_text())["packages"]
        if not isinstance(names, list) or not all(
            isinstance(n, str) and n for n in names
        ):
            raise TypeError("'packages' must be a list of names")
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise ValueError(f"cannot read {path}: {e}") from e
    return list(dict.fromkeys(names))
