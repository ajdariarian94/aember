# aember/utils/download.py
"""File hashes and pinned downloads (verified by sha256, cached)."""

import hashlib
import logging
import shutil
import urllib.request
from pathlib import Path

log = logging.getLogger("aember.download")


def sha256_file(path: Path) -> str:
    """Hex sha256 of @p path, read in 1 MiB chunks."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def fetch_pinned(url: str, digest: str, cache: Path, name: str | None = None) -> Path:
    """Download @p url once into @p cache (as @p name, default: the URL's
    last part), verified by sha256; a cached file with the right hash is
    reused."""
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / (name or url.rsplit("/", 1)[1])
    if path.exists() and sha256_file(path) == digest:
        return path
    log.info("Downloading %s", url)
    tmp = path.with_name(path.name + ".part")
    with urllib.request.urlopen(url, timeout=120) as r, tmp.open("wb") as f:
        shutil.copyfileobj(r, f)
    got = sha256_file(tmp)
    if got != digest:
        tmp.unlink()
        raise RuntimeError(f"{url}: sha256 {got} != pinned {digest}")
    tmp.replace(path)
    return path
