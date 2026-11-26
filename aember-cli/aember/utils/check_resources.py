"""
The resources/ rootfs overlay.

resources/<arch>/rootfs/ is copied over the root filesystem by the pack step.
Its small text files (etc/aember/*.json) are tracked in git. The containers'
packages (var/lib/aember/packages/<name>.aember, doc/aember-format.md) are
too large for git: they come from aember's own GitHub release v<VERSION> as
aember-resources-v<VERSION>.tar.gz (packed by `aember release`).

check_resources() downloads that tarball when a package some board's
containers.json lists is missing, and takes only the missing packages out of
it (the git-tracked configs always win). A release from before packages
holds each container as <name>/<name>.squashfs, lxc.container.conf and an
entry in containers.json: such a container is made into an unsigned package
(for development boards, which allow them). Packages no containers.json
lists aren't needed (they don't go into any image, see
steps/packaging/overlay.py).
"""

import json
import logging
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

from . import aember_package
from .containers import CONTAINERS_JSON, missing_images
from .fetch_artifact import _get_repo, get_version

log = logging.getLogger("aember.check_resources")


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    log.info("Downloading %s", url)
    with urllib.request.urlopen(url) as resp, tmp.open("wb") as out:
        shutil.copyfileobj(resp, out, 1 << 20)
    tmp.replace(dest)


def _extract_missing(tar_path: Path, root: Path, missing: list[Path]) -> int:
    """The missing packages (paths under root) from tar_path: as packages,
    or made from a container of the layout before packages. Returns how
    many it got."""
    count = 0
    wanted = {str(p.relative_to(root)): p for p in missing}
    with tarfile.open(tar_path, "r:gz") as tar, tempfile.TemporaryDirectory() as tmp:
        members = {m.name: m for m in tar.getmembers() if m.isfile()}
        for name, dest in wanted.items():
            dest.parent.mkdir(parents=True, exist_ok=True)
            if name in members:
                with tar.extractfile(members[name]) as src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                count += 1
                continue
            # resources/<arch>/rootfs/var/lib/aember/packages/<c>.aember
            overlay = Path(name).parents[4]
            container = dest.name[: -len(aember_package.EXTENSION)]
            config = str(overlay / CONTAINERS_JSON)
            if config not in members:
                continue
            entries = json.load(tar.extractfile(members[config])).get("containers", [])
            entry = next((e for e in entries if e.get("name") == container), None)
            spec = (entry or {}).get("container", {})
            image = str(overlay / str(spec.get("squashfs", "")).lstrip("/"))
            lxc = str(overlay / str(spec.get("config_path", "")).lstrip("/"))
            if image not in members or lxc not in members:
                continue
            tar.extract(members[image], tmp, filter="tar")
            aember_package.write_package(
                dest,
                aember_package.container_config(container, entry),
                aember_package.container_lxc_config(
                    container, tar.extractfile(members[lxc]).read().decode()
                ),
                Path(tmp) / image,
            )
            log.info("Made %s from the release's %s", dest.relative_to(root), image)
            count += 1
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
        log.info("Listed container packages present — skipping")
        return

    url = f"https://github.com/{_get_repo(root)}/releases/download/v{version}/{name}"
    log.info("Missing %s — fetching %s", _names(missing, root), name)
    try:
        _download(url, tar_path)
        count = _extract_missing(tar_path, root, missing)
    except Exception as e:
        raise RuntimeError(
            f"Could not fetch resource images from {url}: {e}. "
            f"They are needed for the container images in resources/."
        ) from e
    tar_path.unlink(missing_ok=True)
    log.info("Took %d package(s) from %s into resources/", count, name)

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
