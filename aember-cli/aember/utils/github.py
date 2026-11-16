"""
GitHub access through the `gh` CLI.

gh authenticates with GITHUB_TOKEN / GH_TOKEN from the environment, or with a
`gh auth login` session. If neither env var is set, load_github_token() reads
GITHUB_TOKEN from tools/scripts/.env (git-ignored). The token is never logged.
"""

import logging
import os
from pathlib import Path
import re
import subprocess

log = logging.getLogger("aember.github")

ENV_FILE = Path("tools/scripts/.env")


def load_github_token(root: Path) -> None:
    """Put GITHUB_TOKEN from tools/scripts/.env into the env, if not set yet."""
    if os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN"):
        return

    env_path = root / ENV_FILE
    if not env_path.exists():
        log.debug("No GITHUB_TOKEN in env and no %s — relying on gh auth", env_path)
        return

    for line in env_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        line = line.removeprefix("export ").strip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        value = value.strip().strip('"').strip("'")
        if key.strip() == "GITHUB_TOKEN" and value:
            os.environ["GITHUB_TOKEN"] = value
            log.info("Loaded GITHUB_TOKEN from %s", env_path)
            return

    log.debug("No GITHUB_TOKEN in %s — relying on gh auth", env_path)


def get_repo(root: Path) -> str:
    """owner/name of the GitHub repo, taken from the `origin` remote."""
    url = subprocess.run(
        ["git", "-C", str(root), "remote", "get-url", "origin"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    match = re.search(r"github\.com[:/](.+?)(?:\.git)?$", url)
    if not match:
        raise RuntimeError(f"origin is not a GitHub remote: {url}")
    return match.group(1)
