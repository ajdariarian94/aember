"""
Download kernel, BusyBox, LXC and the SDK from the aember-resources release.

For the pinned version (config/resources.py) the step reads the release's
manifest.json and, for every asset of ctx.architecture:
  - skips it if build/aember-resources/<arch>/<component>/.asset-sha256
    already holds the manifest's sha256,
  - otherwise downloads it, checks its sha256, and installs it: a zip is
    extracted (Unix modes restored), the SDK installer is run with -d.

The repo is public, so plain HTTPS downloads are used (no gh, no token).
"""

import hashlib
import json
import logging
from pathlib import Path
import shutil
import stat
import urllib.request
import zipfile

from aember.config.resources import (
    ASSET_MARKER,
    RESOURCES_VERSION,
    component_dir,
    release_url,
)
from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner

log = logging.getLogger("aember.resources")

_CHUNK = 1 << 20


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    log.info("Downloading %s", url)
    with urllib.request.urlopen(url) as resp, tmp.open("wb") as out:
        shutil.copyfileobj(resp, out, _CHUNK)
    tmp.replace(dest)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(_CHUNK):
            h.update(chunk)
    return h.hexdigest()


def _extract_zip(zip_path: Path, dest_dir: Path) -> None:
    dest_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            target = Path(zf.extract(info, dest_dir))
            mode = stat.S_IMODE(info.external_attr >> 16)
            if mode and not info.is_dir():
                target.chmod(mode)


class FetchResources(Step):

    def download_dir(self, ctx: Context) -> Path:
        return (
            ctx.root / "build" / "downloads" / f"aember-resources-v{RESOURCES_VERSION}"
        )

    def _manifest(self, ctx: Context) -> dict:
        path = self.download_dir(ctx) / "manifest.json"
        _download(release_url("manifest.json"), path)
        manifest = json.loads(path.read_text())
        if manifest.get("version") != RESOURCES_VERSION:
            raise RuntimeError(
                f"manifest.json is for {manifest.get('version')}, "
                f"expected {RESOURCES_VERSION}"
            )
        return manifest

    def _install(self, ctx: Context, asset: dict) -> None:
        out = component_dir(ctx.root, ctx.architecture, asset["component"])
        marker = out / ASSET_MARKER
        if marker.is_file() and marker.read_text().strip() == asset["sha256"]:
            log.info("%s up to date — skipping", asset["component"])
            return

        file = self.download_dir(ctx) / asset["name"]
        if not file.is_file() or _sha256(file) != asset["sha256"]:
            _download(release_url(asset["name"]), file)
        if _sha256(file) != asset["sha256"]:
            raise RuntimeError(f"sha256 mismatch for downloaded {asset['name']}")

        shutil.rmtree(out, ignore_errors=True)
        if asset["format"] == "zip":
            _extract_zip(file, out)
        elif asset["format"] == "installer":
            log.info("Installing SDK → %s", out)
            CommandRunner.run(["sh", str(file), "-d", str(out), "-y"])
        else:
            raise RuntimeError(f"Unknown asset format '{asset['format']}'")

        marker.write_text(asset["sha256"] + "\n")
        file.unlink()  # keep build/downloads small; the marker records the hash
        log.info("Installed %s → %s", asset["name"], out)

    def run(self, ctx: Context) -> None:
        log.info(
            "Fetching aember-resources v%s for %s", RESOURCES_VERSION, ctx.architecture
        )
        assets = [
            a
            for a in self._manifest(ctx)["assets"]
            if a["architecture"] == ctx.architecture
        ]
        if not assets:
            raise RuntimeError(
                f"aember-resources v{RESOURCES_VERSION} has no assets for "
                f"'{ctx.architecture}'"
            )
        for asset in assets:
            self._install(ctx, asset)
