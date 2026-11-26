# aember/cli/package/commands.py
"""`aember package`: make, inspect and sign .aember packages
(doc/aember-format.md). Container repositories make theirs with
aember-build-helper (`aember-cli/run.py pack` there); these are for keys,
checks and packages made by hand."""

import json
import logging
from pathlib import Path

import typer  # type: ignore

from aember.utils import aember_package as pkg

log = logging.getLogger("aember.package")


def _fail(e: Exception) -> None:
    log.error("%s", e)
    raise typer.Exit(1)


def keygen(
    name: str = typer.Argument(..., help="Key name: <name>.key and <name>.pub"),
    out: Path = typer.Option(Path("."), "-o", "--out", help="Directory"),
):
    """A new Ed25519 key pair for signing packages. The .pub goes into a
    board's overlay (etc/aember/keys/); keep the .key off the device and
    out of git."""
    try:
        private, public = pkg.keygen(out, name)
    except pkg.PackageError as e:
        _fail(e)
    log.info("Private key: %s (keep it secret)", private)
    log.info("Public key:  %s (into etc/aember/keys/ of a board)", public)


def create(
    config: Path = typer.Option(..., "--config", help="config.json"),
    lxc: Path = typer.Option(..., "--lxc", help="lxc.container.conf"),
    image: Path = typer.Option(..., "--image", help="The SquashFS image"),
    out: Path = typer.Option(None, "-o", "--out", help="Default: <name>.aember"),
    key: Path = typer.Option(None, "--key", help="Sign with this .key"),
):
    """Make a package from its three files."""
    try:
        data = json.loads(config.read_text())
        target = out or Path(f"{data.get('name')}{pkg.EXTENSION}")
        seed = pkg.read_private_key(key) if key else None
        footer = pkg.write_package(target, data, lxc.read_text(), image, seed)
    except (pkg.PackageError, OSError, ValueError) as e:
        _fail(e)
    log.info(
        "%s: %d bytes, %s",
        target,
        target.stat().st_size,
        f"signed ({footer.key_id.hex()})" if footer.signed else "unsigned",
    )


def inspect(
    package: Path = typer.Argument(..., help="The .aember file"),
    keys: Path = typer.Option(
        None, "--keys", help="Check the signature against these .pub files"
    ),
    extract_to: Path = typer.Option(None, "--extract", help="Unpack into here"),
):
    """What a package holds; with --keys, whether a device would take it."""
    try:
        footer = pkg.read_footer(package)
        log.info("%s", package)
        log.info(
            "  payload  %d bytes, SHA-512 %s…",
            footer.payload_size,
            footer.digest.hex()[:32],
        )
        log.info("  key id   %s", footer.key_id.hex() if footer.signed else "unsigned")
        if keys is not None:
            signed_by = pkg.verify(package, pkg.load_keys(keys), allow_unsigned=True)
            log.info(
                "  valid    %s", f"signed by {signed_by}" if signed_by else "unsigned"
            )
        if extract_to is not None:
            config = pkg.extract(package, extract_to)
            log.info("  unpacked into %s: %s", extract_to, json.dumps(config))
    except (pkg.PackageError, OSError) as e:
        _fail(e)
