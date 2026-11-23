"""
Publish aember's release v<VERSION> (CMakeLists.txt project VERSION) to GitHub.

Uploads what `aember release` wrote to build/release/<arch>/<type>/:
  - every aember-app-* and aember-binary-* tarball of the version,
  - the rootfs overlay aember-resources-v<V>.tar.gz (+ .sha256) once: every
    <arch>/<type> dir holds a copy packed from the same resources/ tree,
  - the flashable disk images in build/release/images/ (Pi 4 SD card, x86-64
    USB stick, .img.xz + .sha256) and the Raspberry Pi Imager repository
    (aember-rpi-imager.json + its icon), when `aember release` made them.

Before anything is uploaded the step checks that every overlay tarball still
matches its .sha256 and that HEAD is pushed to origin. The tag v<VERSION> is
created at HEAD. Assets that already exist in the release are refused unless
clobber is set.
"""

import json
import logging
import subprocess
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step
from aember.utils.github import get_repo, load_github_token
from aember.utils.runner import CommandRunner

from .get_version_from_cmake import get_version_from_cmake
from .images import release_images
from aember.utils.download import sha256_file

log = logging.getLogger("aember.publish")


def _git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout.strip()


class PublishRelease(Step):
    def __init__(self, clobber: bool = False) -> None:
        self.clobber = clobber

    # ------------------------------------------------------------------
    # Assets
    # ------------------------------------------------------------------

    def _assets(self, root: Path, version: str) -> list[Path]:
        rel = root / "build" / "release"
        suffix = f"-v{version}.tar.gz"
        tarballs = sorted(
            p
            for p in rel.glob(f"*/*/aember-*{suffix}")
            if p.name.startswith(("aember-app-", "aember-binary-"))
        )
        if not tarballs:
            raise RuntimeError(
                f"No aember-app/-binary tarballs for v{version} in {rel}. "
                f"Run `aember release` first."
            )

        overlays = sorted(rel.glob(f"*/*/aember-resources{suffix}"))
        if not overlays:
            raise RuntimeError(f"No aember-resources{suffix} in {rel}.")
        for o in overlays:
            sha_file = o.with_name(o.name + ".sha256")
            if not sha_file.is_file():
                raise RuntimeError(f"Missing {sha_file}")
            if sha_file.read_text().split()[0] != sha256_file(o):
                raise RuntimeError(f"{o} does not match {sha_file.name}")
        overlay = overlays[0]

        names = [p.name for p in tarballs]
        dupes = sorted({n for n in names if names.count(n) > 1})
        if dupes:
            raise RuntimeError(f"Duplicate asset names: {dupes}")
        images = release_images(root, version)
        for img in (i for i in images if i.name.endswith(".img.xz")):
            sha_file = img.with_name(img.name + ".sha256")
            if sha_file.read_text().split()[0] != sha256_file(img):
                raise RuntimeError(f"{img} does not match {sha_file.name}")
        return [
            *tarballs,
            overlay,
            overlay.with_name(overlay.name + ".sha256"),
            *images,
        ]

    # ------------------------------------------------------------------
    # Checks and GitHub
    # ------------------------------------------------------------------

    @staticmethod
    def _check_head(root: Path) -> str:
        head = _git(root, "rev-parse", "HEAD")
        if not _git(root, "branch", "-r", "--contains", head):
            raise RuntimeError(
                f"HEAD {head[:12]} is not pushed to origin. Commit, push, "
                f"then publish."
            )
        return head

    @staticmethod
    def _existing_assets(repo: str, tag: str) -> set[str] | None:
        """Asset names of the release, or None if it doesn't exist."""
        res = subprocess.run(
            ["gh", "release", "view", tag, "--repo", repo, "--json", "assets"],
            capture_output=True,
            text=True,
        )
        if res.returncode != 0:
            if "release not found" in res.stderr.lower():
                return None
            raise RuntimeError(f"gh release view {tag} failed: {res.stderr.strip()}")
        return {a["name"] for a in json.loads(res.stdout)["assets"]}

    @staticmethod
    def _notes(root: Path, version: str, head: str, assets: list[Path]) -> str:
        """The changelog written by `aember changelog` for this version."""
        marker = f"→ `{version}`"
        for path in sorted((root / "doc" / "changelog").glob("*.md"), reverse=True):
            text = path.read_text()
            if marker in text:
                return text
        rows = "\n".join(f"- `{a.name}`" for a in assets)
        return f"aember v{version} (commit `{head[:12]}`).\n\n{rows}\n"

    def run(self, ctx: Context) -> None:
        root = ctx.root
        version = get_version_from_cmake(str(root / "CMakeLists.txt"))
        tag = f"v{version}"

        assets = self._assets(root, version)
        head = self._check_head(root)

        load_github_token(root)
        repo = get_repo(root)
        existing = self._existing_assets(repo, tag)

        if existing is None:
            log.info("Creating release %s at %s in %s", tag, head[:12], repo)
            CommandRunner.run(
                [
                    "gh",
                    "release",
                    "create",
                    tag,
                    "--repo",
                    repo,
                    "--target",
                    head,
                    "--title",
                    tag,
                    "--notes",
                    self._notes(root, version, head, assets),
                ]
            )
            existing = set()

        clash = sorted({a.name for a in assets} & existing)
        if clash and not self.clobber:
            raise RuntimeError(
                f"Release {tag} already has {clash}. Use --clobber to replace them."
            )

        log.info("Uploading %d files to %s %s", len(assets), repo, tag)
        CommandRunner.run(
            [
                "gh",
                "release",
                "upload",
                tag,
                *map(str, assets),
                "--repo",
                repo,
                "--clobber",
            ]
        )
        log.info("Published %s: https://github.com/%s/releases/tag/%s", tag, repo, tag)
