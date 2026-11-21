"""
The resources/ rootfs overlay.

resources/<arch>/rootfs/ is copied over the root filesystem by the pack step.
Its small text files (etc/aember/*.json, lxc.container.conf)
are tracked in git. The container images (*.squashfs) are too large for
git: they come from aember's own GitHub
release v<VERSION> as aember-resources-v<VERSION>.tar.gz (packed by
`aember release`).

check_resources() downloads that tarball when an image some board's
containers.json lists is missing, and extracts only files that don't exist
yet, so the git-tracked configs always win over the (possibly older) copies
in the tarball. Images no containers.json lists aren't needed (they don't go
into any image, see steps/packaging/overlay.py).
"""

import logging
from pathlib import Path
import shutil
import tarfile
import urllib.request

from .containers import missing_images
from .fetch_artifact import _get_repo, get_version

log = logging.getLogger("aember.check_resources")


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


def _missing(resources_dir: Path) -> list[Path]:
    """Listed images missing in any board's overlay (resources/<arch>/rootfs)."""
    missing: list[Path] = []
    for overlay in sorted(resources_dir.glob("*/rootfs")):
        try:
            missing += missing_images(overlay)
        except ValueError as e:  # the pack step fails on it with the same error
            log.warning("%s", e)
    return missing


def _names(paths: list[Path], root: Path) -> str:
    return ", ".join(str(p.relative_to(root)) for p in paths)


def check_resources(root: Path) -> None:
    """Make sure resources/ has the listed container images; raise if the
    download fails."""
    resources_dir = root / "resources"
    version = get_version(root)
    name = f"aember-resources-v{version}.tar.gz"
    tar_path = root / "build" / "downloads" / name

    missing = _missing(resources_dir)
    if not missing:
        log.info("Listed container images present — skipping")
        return

    url = f"https://github.com/{_get_repo(root)}/releases/download/v{version}/{name}"
    log.info("Missing %s — fetching %s", _names(missing, root), name)
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

    still = _missing(resources_dir)
    if still:
        # Not released yet (a new container): `aember app pack` fails for
        # that board until the image is installed from its own repository.
        log.warning(
            "Not in %s either: %s. Install them from their repository "
            "(aember-cli/run.py install -a <arch> <aember>).",
            name,
            _names(still, root),
        )
