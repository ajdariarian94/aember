import logging
import os
from pathlib import Path

from .fetch_artifact import fetch_artifact, get_version

log = logging.getLogger("aember.check_resources")

ENV_FILE = Path("tools/scripts/.env")


def _load_github_token(root: Path) -> None:
    """
    Ensure GITHUB_TOKEN is set in the environment so `gh` is authenticated,
    without requiring an interactive `gh auth login`.
    Reads tools/scripts/.env (export GITHUB_TOKEN="...") if the token isn't
    already present in the environment.
    """
    if os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN"):
        return

    env_path = root / ENV_FILE
    if not env_path.exists():
        log.warning(
            "No GITHUB_TOKEN in env and %s not found — gh may fail to auth", env_path
        )
        return

    for line in env_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        line = line.removeprefix("export ").strip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key == "GITHUB_TOKEN" and value:
            os.environ["GITHUB_TOKEN"] = value
            log.info("Loaded GITHUB_TOKEN from %s", env_path)
            return

    log.warning("GITHUB_TOKEN not found in %s", env_path)


def _resources_present(resources_dir: Path) -> bool:
    return resources_dir.exists() and any(resources_dir.iterdir())


def check_resources(root: Path) -> bool:
    """
    Ensure resources/ is present locally.
    If missing/empty, downloads and extracts the aember-resources artifact
    from GitHub Releases (produced by ReleaseStep._pack_resources). If
    already present, skips.
    Returns True if resources are present/fetched successfully, False otherwise.
    """
    _load_github_token(root)

    resources_dir = root / "resources"
    log.info("Checking resources_dir=%s (root=%s)", resources_dir, root)

    if _resources_present(resources_dir):
        log.info("Resources already present — skipping")
        return True

    log.info("Resources missing — fetching from release")

    version = get_version(root)
    artifact_name = f"aember-resources-v{version}.tar.gz"

    # The tarball's top-level entry is "resources", so extract into root
    # itself rather than into resources_dir.
    return fetch_artifact(root, artifact_name, root)
