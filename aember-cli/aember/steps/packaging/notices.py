"""
Third-party notices on the image: /usr/share/licenses/aember/NOTICES.gz.

doc/licensing/components.json lists what aember can put on a device. The
pack step keeps the components that are present in this image:

- ``detect``: globs relative to the image root, checked in every staged tree
  (``"*"``: always, e.g. the kernel);
- ``packages``: Yocto package names, present when a listed opt-in package's
  packages.txt names one of them.

For those it writes, gzip-compressed (every KB counts on a device), the
source offer (doc/licensing/SOURCE-OFFER.md), one entry per component
(name, version, licence, how it's linked, what we must do), the upstream
copyright files vcpkg installed for the libraries compiled into aember, and
each licence text once (doc/licensing/texts/). `zcat
/usr/share/licenses/aember/NOTICES.gz` reads it on the device.

This is the engineering half of licence compliance (see
doc/licensing/LICENSING-NOTES.md); it never decides what may ship.
"""

import fnmatch
import gzip
import json
import logging
from dataclasses import dataclass, field
from pathlib import Path

log = logging.getLogger("aember.pack.notices")

LICENSING_DIR = Path("doc") / "licensing"
COMPONENTS_JSON = LICENSING_DIR / "components.json"
TEXTS_DIR = LICENSING_DIR / "texts"
SOURCE_OFFER = LICENSING_DIR / "SOURCE-OFFER.md"
NOTICES_PATH = Path("usr") / "share" / "licenses" / "aember" / "NOTICES.gz"


@dataclass
class Component:
    id: str
    name: str
    version: str
    license: str
    where: str
    linking: str
    obligation: str
    url: str
    texts: list[str]
    detect: list[str] = field(default_factory=list)
    packages: list[str] = field(default_factory=list)
    vcpkg: str | None = None
    note: str | None = None


def load_components(root: Path) -> list[Component]:
    """components.json, checked: unknown keys, missing texts and duplicate
    ids are errors (a typo would silently drop a notice)."""
    data = json.loads((root / COMPONENTS_JSON).read_text())
    known = set(Component.__dataclass_fields__)
    out: list[Component] = []
    seen: set[str] = set()
    for raw in data["components"]:
        unknown = set(raw) - known
        if unknown:
            raise ValueError(f"{COMPONENTS_JSON}: {raw.get('id')}: unknown {unknown}")
        c = Component(**raw)
        if c.id in seen:
            raise ValueError(f"{COMPONENTS_JSON}: duplicate id {c.id}")
        seen.add(c.id)
        for t in c.texts:
            if not (root / TEXTS_DIR / f"{t}.txt").is_file():
                raise ValueError(f"{COMPONENTS_JSON}: {c.id}: no text {t}")
        out.append(c)
    return out


def _files(trees: list[Path]) -> list[str]:
    """Every path in the trees, relative to its tree root (posix)."""
    paths: list[str] = []
    for tree in trees:
        if not tree.is_dir():
            continue
        for p in tree.rglob("*"):
            paths.append(p.relative_to(tree).as_posix())
    return paths


def present(
    components: list[Component], trees: list[Path], packages: set[str]
) -> list[Component]:
    """The components found in the staged trees or the packages."""
    files = _files(trees)
    found: list[Component] = []
    for c in components:
        hit = any(pat == "*" for pat in c.detect)
        hit = hit or any(fnmatch.filter(files, pat) for pat in c.detect)
        hit = hit or bool(packages.intersection(c.packages))
        if hit:
            found.append(c)
    return found


def installed_packages(package_dirs: list[Path]) -> set[str]:
    """Yocto package names from the packages.txt of each opt-in package
    ("<name> <arch> <version>" per line)."""
    names: set[str] = set()
    for d in package_dirs:
        txt = d / "packages.txt"
        if txt.is_file():
            for line in txt.read_text().splitlines():
                if line.strip():
                    names.add(line.split()[0])
    return names


def render(root: Path, components: list[Component], vcpkg_share: Path | None) -> str:
    """The NOTICES text."""
    parts = [
        "Third-party software on this aember device\n"
        "==========================================\n\n"
        "aember itself is under the Aember License (see the vendor's\n"
        "documentation). This device also contains the software below, each\n"
        "under its own licence; the licence texts follow the list.\n",
        (root / SOURCE_OFFER).read_text() if (root / SOURCE_OFFER).is_file() else "",
        "\nComponents\n----------\n",
    ]
    for c in components:
        parts.append(
            f"\n* {c.name} {c.version}\n"
            f"  Licence: {c.license}\n"
            f"  Where: {c.where}; {c.linking}\n"
            f"  Source: {c.url}\n"
        )
    copyrights: list[str] = []
    if vcpkg_share is not None:
        for c in components:
            f = vcpkg_share / (c.vcpkg or "") / "copyright"
            if c.vcpkg and f.is_file():
                copyrights.append(f"\n--- {c.name} ---\n\n{f.read_text().strip()}\n")
    if copyrights:
        parts.append("\nCopyright notices of compiled-in libraries\n")
        parts.append("------------------------------------------\n")
        parts.extend(copyrights)
    parts.append("\nLicence texts\n-------------\n")
    for t in sorted({t for c in components for t in c.texts}):
        text = (root / TEXTS_DIR / f"{t}.txt").read_text().strip()
        parts.append(f"\n=== {t} ===\n\n{text}\n")
    return "".join(parts)


def write_notices(
    root: Path,
    trees: list[Path],
    package_dirs: list[Path],
    out_root: Path,
    vcpkg_share: Path | None = None,
) -> Path:
    """Write NOTICES.gz under @p out_root (a tree that goes into the
    rootfs) for what @p trees and @p package_dirs contain."""
    components = present(load_components(root), trees, installed_packages(package_dirs))
    text = render(root, components, vcpkg_share)
    out = out_root / NOTICES_PATH
    out.parent.mkdir(parents=True, exist_ok=True)
    # mtime=0: the same image content gives the same file.
    with gzip.GzipFile(out, "wb", compresslevel=9, mtime=0) as f:
        f.write(text.encode())
    log.info(
        "Third-party notices: %d components, %d bytes (%s)",
        len(components),
        out.stat().st_size,
        NOTICES_PATH,
    )
    return out
