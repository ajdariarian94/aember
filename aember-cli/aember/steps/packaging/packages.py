"""
The opt-in packages a board lists (etc/aember/packages.json), as one tree.

stage_packages() writes build/package/<arch>/<type>/packages_root: the
rootfs/ of every listed package from build/aember-resources/<arch>/packages/,
their symlinks (symlinks.txt; release zips can't hold links) and empty
directories (dirs.txt, e.g. an SSH server's host key directory),
and a minimal user database (/etc/passwd, /etc/group, /root) for programs
that look users up, such as an SSH server, plus the system users packages
bring (users.txt, groups.txt: D-Bus's messagebus). The tree goes into the root
filesystem only (never the initramfs), and the board's overlay is copied
after it, so a board's own /etc/passwd wins. A package ships everything
but the C library; the pack step adds what its programs need of that from
the SDK (package_executables + copy_runtime_libs).
"""

import logging
import shutil
from pathlib import Path

from aember.config.resources import package_dir
from aember.utils.packages import listed_packages

from .libs import get_interpreter

log = logging.getLogger("aember.pack.packages")

PASSWD = "root:x:0:0:root:/root:/bin/sh\n"
GROUP = "root:x:0:\n"


def _add_symlinks(tree: Path, symlinks_txt: Path, name: str) -> int:
    count = 0
    if not symlinks_txt.is_file():
        return count
    for line in symlinks_txt.read_text().splitlines():
        if not line.strip():
            continue
        # "<path>\t<target>": a tab, since firmware names have spaces.
        path, _, target = line.partition("\t")
        if not path.startswith("/") or not target:
            raise ValueError(f"{symlinks_txt}: bad line {line!r}")
        link = tree / path.lstrip("/")
        link.parent.mkdir(parents=True, exist_ok=True)
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(target)
        count += 1
    log.debug("%s: %d symlinks", name, count)
    return count


def _add_dirs(tree: Path, dirs_txt: Path) -> None:
    if not dirs_txt.is_file():
        return
    for line in dirs_txt.read_text().splitlines():
        if not line.strip():
            continue
        path, _, mode = line.partition("\t")
        if not path.startswith("/") or not mode:
            raise ValueError(f"{dirs_txt}: bad line {line!r}")
        d = tree / path.lstrip("/")
        d.mkdir(parents=True, exist_ok=True)
        d.chmod(int(mode, 8))


def package_executables(tree: Path) -> list[Path]:
    """ELF executables in tree (files with a program interpreter)."""
    found = []
    for path in sorted(tree.rglob("*")):
        if path.is_symlink() or not path.is_file() or not path.stat().st_mode & 0o111:
            continue
        with path.open("rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
        try:
            if get_interpreter(path):
                found.append(path)
        except RuntimeError:  # shared libraries have no interpreter
            continue
    return found


def _add_entries(file: Path, lines_txt: Path) -> None:
    """Append the passwd/group lines of a package (users.txt, groups.txt)
    whose name file doesn't have yet."""
    if not lines_txt.is_file():
        return
    have = {line.split(":")[0] for line in file.read_text().splitlines() if line}
    new = [
        line
        for line in lines_txt.read_text().splitlines()
        if ":" in line and line.split(":")[0] not in have
    ]
    if new:
        with file.open("a") as f:
            f.writelines(line + "\n" for line in new)


def _write_users(tree: Path) -> None:
    etc = tree / "etc"
    etc.mkdir(parents=True, exist_ok=True)
    (etc / "passwd").write_text(PASSWD)
    (etc / "group").write_text(GROUP)
    (tree / "root").mkdir(mode=0o700, exist_ok=True)


def stage_packages(root: Path, architecture: str, overlay: Path, dest: Path) -> Path:
    """
    Build dest from the packages the overlay's packages.json lists. Raises
    FileNotFoundError for a listed package that isn't installed (FetchResources
    installs them) and ValueError for a malformed packages.json.
    """
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    _write_users(dest)

    names = listed_packages(overlay)
    for name in names:
        src = package_dir(root, architecture, name)
        rootfs = src / "rootfs"
        if not rootfs.is_dir():
            raise FileNotFoundError(
                f"packages.json lists '{name}', but {rootfs} doesn't exist: "
                "`aember app build` (FetchResources) installs it."
            )
        shutil.copytree(
            rootfs,
            dest,
            symlinks=True,
            # Copies, not hard links: Release packing strips lib/ in place.
            copy_function=shutil.copy2,
            dirs_exist_ok=True,
        )
        _add_symlinks(dest, src / "symlinks.txt", name)
        _add_dirs(dest, src / "dirs.txt")
        _add_entries(dest / "etc" / "passwd", src / "users.txt")
        _add_entries(dest / "etc" / "group", src / "groups.txt")
    log.info("Packages packed: %s", ", ".join(names) or "none")
    return dest
