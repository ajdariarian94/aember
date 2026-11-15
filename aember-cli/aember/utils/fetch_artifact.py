import logging
import re
import tarfile
from pathlib import Path

from aember.utils.runner import CommandRunner

log = logging.getLogger("aember.fetch_artifact")


def _get_repo(root: Path) -> str:
    cmake = (root / "CMakeLists.txt").read_text()
    match = re.search(r'HOMEPAGE_URL\s+"([^"]+)"', cmake)
    if not match:
        raise RuntimeError("HOMEPAGE_URL not found in CMakeLists.txt")
    repo_url = match.group(1).removesuffix(".git")
    return repo_url.replace("https://github.com/", "")


def get_version(root: Path) -> str:
    cmake = (root / "CMakeLists.txt").read_text()
    match = re.search(r"project\s*\([^)]*VERSION\s+([\d.]+)", cmake, re.DOTALL)
    if not match:
        raise RuntimeError("Project VERSION not found in CMakeLists.txt")
    return match.group(1)


def download_release_asset(root: Path, asset_name: str, dest_dir: Path) -> Path:
    """
    Download a GitHub Release asset into dest_dir using gh CLI.
    Returns the path to the downloaded file.
    """
    repo = _get_repo(root)
    version = get_version(root)
    dest_dir.mkdir(parents=True, exist_ok=True)

    log.info("Downloading release asset: %s", asset_name)

    CommandRunner.run(
        f"gh release download v{version} "
        f"--repo {repo} "
        f"--pattern '{asset_name}' "
        f"--dir {dest_dir} "
        f"--clobber",
        shell=True,
    )

    return dest_dir / asset_name


def extract_tar(tar_path: Path, dest_dir: Path) -> None:
    """Extract a tar.gz into dest_dir and remove the archive."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tar_path, "r:gz") as tar:
        tar.extractall(dest_dir)
    tar_path.unlink()
    log.debug("Extracted to %s", dest_dir)


def fetch_artifact(root: Path, artifact_name: str, dest_dir: Path) -> bool:
    """
    Download and extract a named release artifact into dest_dir.
    Returns True if succeeded, False otherwise.
    """
    try:
        # Download into a temp location inside build/downloads/
        download_dir = root / "build" / "downloads"
        tar_path = download_release_asset(root, artifact_name, download_dir)
        extract_tar(tar_path, dest_dir)
        log.info("Fetched artifact: %s → %s", artifact_name, dest_dir)
        return True
    except Exception as e:
        log.warning("Failed to fetch artifact '%s': %s", artifact_name, e)
        return False
