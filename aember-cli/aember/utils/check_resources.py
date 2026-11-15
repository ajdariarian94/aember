"""
The resources/ rootfs overlay.

resources/<arch>/rootfs/ is copied over the root filesystem by the pack step.
Its small text files (etc/aember/*.json, lxc.container.conf, udhcpc script)
are tracked in git. The container images (*.squashfs, e.g. the Qt-based
aember-monitor) are too large for git: they come from aember's own GitHub
release v<VERSION> as aember-resources-v<VERSION>.tar.gz (packed by
`aember release`).

check_resources() downloads that tarball when an image is missing and
extracts only files that don't exist yet, so the git-tracked configs always
win over the (possibly older) copies in the tarball.
"""

import logging
from pathlib import Path
import shutil
import tarfile
import urllib.request

from .fetch_artifact import _get_repo, get_version

log = logging.getLogger("aember.check_resources")

IMAGE_GLOB = "*.squashfs"


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    log.info("Downloading %s", url)
    with urllib.request.urlopen(url) as resp, tmp.open("wb") as out:
        shutil.copyfileobj(resp, out, 1 << 20)
    tmp.replace(dest)


def _extract_missing(tar_path: Path, root: Path) -> int:
    """Extract members of tar_path under root that don't exist yet."""
    count = 0
    with tarfile.open(tar_path, "r:gz") as tar:
        for member in tar.getmembers():
            if not member.name.startswith("resources/"):
                continue
            if member.isfile() and (root / member.name).exists():
                log.debug("Keeping existing %s", member.name)
                continue
            tar.extract(member, root, filter="tar")
            count += member.isfile()
    return count


def check_resources(root: Path) -> None:
    """Make sure resources/ has its container images; raise if that fails."""
    resources_dir = root / "resources"
    version = get_version(root)
    name = f"aember-resources-v{version}.tar.gz"
    tar_path = root / "build" / "downloads" / name

    if any(resources_dir.rglob(IMAGE_GLOB)):
        log.info("Resource images present — skipping")
        return

    url = f"https://github.com/{_get_repo(root)}/releases/download/v{version}/{name}"
    log.info("Resource images missing — fetching %s", name)
    try:
        _download(url, tar_path)
        count = _extract_missing(tar_path, root)
    except Exception as e:
        raise RuntimeError(
            f"Could not fetch resource images from {url}: {e}. "
            f"They are needed for the container images in resources/."
        ) from e
    tar_path.unlink(missing_ok=True)
    log.info("Extracted %d file(s) from %s into resources/", count, name)
