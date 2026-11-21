"""
The rootfs overlay as a board's image gets it.

resources/<arch>/rootfs/ may hold more container images than the board runs
(var/lib/aember/<name>/), but only the containers its containers.json lists
go into the image: the others would only take space. stage_overlay() writes
that subset to build/package/<arch>/<type>/overlay_root, which the rootfs
image, the Raspberry Pi and USB images and the AppImage all use instead of
resources/<arch>/rootfs/ itself. Everything outside var/lib/aember/<name>/
container directories is copied unchanged.
"""

import logging
import os
import shutil
from pathlib import Path

from aember.utils.containers import IMAGES_DIR, container_files

log = logging.getLogger("aember.pack.overlay")


def _is_container_dir(path: Path) -> bool:
    return path.is_dir() and (
        any(path.glob("*.squashfs")) or (path / "lxc.container.conf").exists()
    )


def _link_or_copy(src: str, dst: str) -> None:
    # Hard links: the staged tree costs no copy of the (large) images.
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def stage_overlay(overlay: Path, dest: Path) -> Path:
    """
    Copy overlay to dest without the container directories containers.json
    doesn't list. Raises FileNotFoundError when a listed container's image
    or config is missing: the image would ship a container that can't start.
    """
    if dest.exists():
        shutil.rmtree(dest)
    if not overlay.exists():
        log.debug("No overlay directory at %s", overlay)
        dest.mkdir(parents=True)
        return dest

    files = container_files(overlay)
    missing = [f for f in files if not f.exists()]
    if missing:
        raise FileNotFoundError(
            "containers.json lists files the overlay doesn't have: "
            + ", ".join(str(f.relative_to(overlay)) for f in missing)
            + ". Released images come with `aember app build` (CheckResources); "
            "a new container: `aember-cli/run.py install -a <arch> <this repo>` "
            "in its own repository."
        )

    keep = {f.parent for f in files}
    images_dir = overlay / IMAGES_DIR
    dropped: list[str] = []

    def ignore(directory: str, names: list[str]) -> list[str]:
        if Path(directory) != images_dir:
            return []
        skip = [
            n
            for n in names
            if _is_container_dir(images_dir / n) and images_dir / n not in keep
        ]
        dropped.extend(skip)
        return skip

    shutil.copytree(
        overlay, dest, symlinks=True, ignore=ignore, copy_function=_link_or_copy
    )
    kept = sorted(p.name for p in keep if p.parent == images_dir)
    log.info(
        "Containers packed: %s%s",
        ", ".join(kept) or "none",
        f" (not listed, left out: {', '.join(sorted(dropped))})" if dropped else "",
    )
    return dest
