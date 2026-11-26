"""
The rootfs overlay as a board's image gets it.

resources/<arch>/rootfs/ may hold more packages than the board runs
(var/lib/aember/packages/<name>.aember), but only the containers its
containers.json lists go into the image, and installed already, as a device
installs a package (var/lib/aember/<name>/: config.json,
lxc.container.conf, image.squashfs, package.json; doc/aember-format.md): the
image doesn't carry each container twice, and its first boot doesn't spend
time unpacking. stage_overlay() writes that to
build/package/<arch>/<type>/overlay_root, which the rootfs image, the
Raspberry Pi and USB images and the AppImage all use instead of
resources/<arch>/rootfs/ itself. Everything else is copied unchanged.
"""

import logging
import os
import shutil
from pathlib import Path

from aember.utils import aember_package
from aember.utils.containers import (
    INSTALL_DIR,
    KEYS_DIR,
    PACKAGES_DIR,
    container_names,
    package_path,
    read_containers_json,
)

log = logging.getLogger("aember.pack.overlay")


def _link_or_copy(src: str, dst: str) -> None:
    # Hard links: the staged tree costs no copy of large files.
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def stage_overlay(overlay: Path, dest: Path) -> Path:
    """
    Copy overlay to dest with the listed packages installed and no package
    files. Raises FileNotFoundError when a listed container's package is
    missing (the image would list a container that can't start), and
    aember_package.PackageError when one isn't valid or isn't signed by a
    key the board trusts.
    """
    if dest.exists():
        shutil.rmtree(dest)
    if not overlay.exists():
        log.debug("No overlay directory at %s", overlay)
        dest.mkdir(parents=True)
        return dest

    names = container_names(overlay)
    missing = [n for n in names if not package_path(overlay, n).exists()]
    if missing:
        raise FileNotFoundError(
            "containers.json lists containers whose package isn't in the "
            "overlay: "
            + ", ".join(
                str(package_path(overlay, n).relative_to(overlay)) for n in missing
            )
            + ". Released ones come with `aember app build` (CheckResources); "
            "a new container: `aember-cli/run.py install -a <arch> <this repo>` "
            "in its own repository."
        )

    packages_dir = overlay / PACKAGES_DIR
    install_dir = overlay / INSTALL_DIR
    stale: list[str] = []

    def ignore(directory: str, entries: list[str]) -> list[str]:
        if Path(directory) == packages_dir:
            return entries
        if Path(directory) == install_dir:
            # Container directories of the layout before packages
            # (<name>/<name>.squashfs, git-ignored leftovers): not packed.
            old = [e for e in entries if any((install_dir / e).glob("*.squashfs"))]
            stale.extend(old)
            return old
        return []

    shutil.copytree(
        overlay, dest, symlinks=True, ignore=ignore, copy_function=_link_or_copy
    )
    # An empty packages directory stays: where a device's updates arrive.
    (dest / PACKAGES_DIR).mkdir(parents=True, exist_ok=True)

    keys = aember_package.load_keys(overlay / KEYS_DIR)
    allow_unsigned = bool(read_containers_json(overlay).get("allow_unsigned", False))
    for name in names:
        installed = aember_package.install(
            package_path(overlay, name), dest / INSTALL_DIR, keys, allow_unsigned
        )
        log.debug("Installed %s (%s)", name, installed["signed_by"] or "unsigned")
    left_out = (
        sorted(
            p.name[: -len(aember_package.EXTENSION)]
            for p in packages_dir.glob(f"*{aember_package.EXTENSION}")
            if p.name[: -len(aember_package.EXTENSION)] not in names
        )
        if packages_dir.is_dir()
        else []
    )
    if stale:
        log.warning(
            "Left out %s: container directories of the layout before .aember "
            "packages (delete them; the packages replace them)",
            ", ".join(str(INSTALL_DIR / n) for n in sorted(stale)),
        )
    log.info(
        "Containers installed: %s%s",
        ", ".join(names) or "none",
        f" (not listed, left out: {', '.join(left_out)})" if left_out else "",
    )
    return dest
