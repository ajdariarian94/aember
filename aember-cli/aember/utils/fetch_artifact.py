"""
Repository facts read from CMakeLists.txt (GitHub repo, app version).
Downloads use plain HTTPS elsewhere (check_resources, fetch_resources);
the old gh-based download helpers that lived here are gone.
"""

import re
from pathlib import Path


def _get_repo(root: Path) -> str:
    cmake = (root / "CMakeLists.txt").read_text()
    match = re.search(r'HOMEPAGE_URL\s+"([^"]+)"', cmake)
    if not match:
        raise RuntimeError("HOMEPAGE_URL not found in CMakeLists.txt")
    repo_url = match.group(1).removesuffix(".git")
    return repo_url.replace("https://github.com/", "")


def get_version(root: Path) -> str:
    cmake = (root / "CMakeLists.txt").read_text()
    match = re.search(r"project\s*\([^)]*VERSION\s+([\d.]+)", cmake, re.DOTALL)
    if not match:
        raise RuntimeError("Project VERSION not found in CMakeLists.txt")
    return match.group(1)
