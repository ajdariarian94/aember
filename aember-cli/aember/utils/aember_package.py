"""
`.aember` packages (doc/aember-format.md): one container in one compressed,
signed file. The device side is libraries/utils/package (C++); this writes
and reads the same format for the tools (`aember package`, the release
conversion, system tests). aember-build-helper has the same module for the
container repositories.

    <payload: one xz stream of a ustar tar>  <footer: 160 bytes>

The tar holds config.json, lxc.container.conf and image.squashfs, in that
order and nothing else.
"""

from __future__ import annotations

import base64
import hashlib
import io
import json
import lzma
import os
import struct
import tarfile
from dataclasses import dataclass
from pathlib import Path

from . import ed25519

MAGIC = b"AEMBERv1"
END_MAGIC = b"1vREBMEA"
FOOTER_SIZE = 160
SIGNATURE_CONTEXT = b"aember-package-v1\0"
EXTENSION = ".aember"
MEMBERS = ("config.json", "lxc.container.conf", "image.squashfs")
XZ_PRESET = 6  # an 8 MiB dictionary: about 9 MiB of memory to unpack
# What a container's config.json may say besides "name" and "command".
CONTAINER_SETTINGS = (
    "init",
    "stop_timeout",
    "restart_policy",
    "max_restart_attempts",
    "restart_delay",
)


class PackageError(Exception):
    pass


# ---------------------------------------------------------------------------
# Keys
# ---------------------------------------------------------------------------


def key_id(public: bytes) -> bytes:
    return hashlib.sha512(public).digest()[:8]


def public_key_text(public: bytes) -> str:
    return base64.b64encode(public).decode() + "\n"


def read_public_key(path: Path) -> bytes:
    try:
        key = base64.b64decode(path.read_text().strip(), validate=True)
    except ValueError as e:
        raise PackageError(f"{path}: not a base64 key ({e})") from e
    if len(key) != 32:
        raise PackageError(f"{path}: not an Ed25519 public key (32 bytes)")
    return key


def read_private_key(path: Path) -> bytes:
    """A .key file: the 32-byte seed in base64."""
    try:
        seed = base64.b64decode(path.read_text().strip(), validate=True)
    except ValueError as e:
        raise PackageError(f"{path}: not a base64 key ({e})") from e
    if len(seed) != 32:
        raise PackageError(f"{path}: not an Ed25519 private key (32 bytes)")
    return seed


def keygen(directory: Path, name: str) -> tuple[Path, Path]:
    """A new key pair: <name>.key (private, 0600) and <name>.pub."""
    directory.mkdir(parents=True, exist_ok=True)
    seed = os.urandom(32)
    private, public = directory / f"{name}.key", directory / f"{name}.pub"
    if private.exists() or public.exists():
        raise PackageError(f"{private} or {public} exists already")
    fd = os.open(private, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(base64.b64encode(seed).decode() + "\n")
    public.write_text(public_key_text(ed25519.public_key(seed)))
    return private, public


def load_keys(directory: Path) -> dict[str, bytes]:
    """name -> public key for every <name>.pub in @p directory."""
    if not directory.is_dir():
        return {}
    return {p.stem: read_public_key(p) for p in sorted(directory.glob("*.pub"))}


# ---------------------------------------------------------------------------
# Footer
# ---------------------------------------------------------------------------


@dataclass
class Footer:
    payload_size: int
    digest: bytes
    key_id: bytes = bytes(8)
    signature: bytes = bytes(64)

    @property
    def signed(self) -> bool:
        return self.key_id != bytes(8)

    def to_bytes(self) -> bytes:
        return self._signed_part() + self.signature + END_MAGIC

    def _signed_part(self) -> bytes:
        return MAGIC + struct.pack("<Q", self.payload_size) + self.digest + self.key_id

    def signed_message(self) -> bytes:
        return SIGNATURE_CONTEXT + self._signed_part()

    @staticmethod
    def parse(raw: bytes) -> Footer:
        if len(raw) != FOOTER_SIZE or raw[:8] != MAGIC or raw[152:] != END_MAGIC:
            raise PackageError("not an .aember package (no AEMBERv1 footer)")
        f = Footer(
            struct.unpack("<Q", raw[8:16])[0], raw[16:80], raw[80:88], raw[88:152]
        )
        if not f.signed and f.signature != bytes(64):
            raise PackageError("a signature without a key id")
        return f


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------


class _CompressingWriter(io.RawIOBase):
    """What tarfile writes: compressed to @p out, hashed as it goes."""

    def __init__(self, out) -> None:
        self.out = out
        self.xz = lzma.LZMACompressor(
            format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=XZ_PRESET
        )
        self.sha = hashlib.sha512()
        self.size = 0

    def writable(self) -> bool:
        return True

    def _emit(self, data: bytes) -> None:
        if data:
            self.out.write(data)
            self.sha.update(data)
            self.size += len(data)

    def write(self, data) -> int:
        self._emit(self.xz.compress(bytes(data)))
        return len(data)

    def finish(self) -> None:
        self._emit(self.xz.flush())


def _member(name: str, size: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime = size, 0o644, 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    return info


def write_package(
    out: Path,
    config: dict,
    lxc_config: str,
    image: Path,
    seed: bytes | None = None,
) -> Footer:
    """
    Write @p out (<name>.aember): @p config (config.json, its "name" must be
    the file's), @p lxc_config, the SquashFS @p image; signed with @p seed
    (a private key) unless None. Reproducible: the same inputs give the
    same bytes.
    """
    name = config.get("name")
    if out.name != f"{name}{EXTENSION}":
        raise PackageError(f"{out.name}: a package of '{name}' is {name}{EXTENSION}")
    config_bytes = (json.dumps(config, indent=2) + "\n").encode()
    lxc_bytes = lxc_config.encode()
    tmp = out.with_name(out.name + ".tmp")
    with open(tmp, "wb") as f:
        writer = _CompressingWriter(f)
        with tarfile.open(
            fileobj=writer, mode="w|", format=tarfile.USTAR_FORMAT
        ) as tar:
            tar.addfile(
                _member(MEMBERS[0], len(config_bytes)), io.BytesIO(config_bytes)
            )
            tar.addfile(_member(MEMBERS[1], len(lxc_bytes)), io.BytesIO(lxc_bytes))
            with open(image, "rb") as img:
                tar.addfile(_member(MEMBERS[2], os.fstat(img.fileno()).st_size), img)
        writer.finish()
        footer = Footer(writer.size, writer.sha.digest())
        if seed is not None:
            footer.key_id = key_id(ed25519.public_key(seed))
            footer.signature = ed25519.sign(seed, footer.signed_message())
        f.write(footer.to_bytes())
    tmp.replace(out)
    return footer


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------


def read_footer(path: Path) -> Footer:
    size = path.stat().st_size
    if size < FOOTER_SIZE:
        raise PackageError(f"{path}: too small for an .aember package")
    with open(path, "rb") as f:
        f.seek(size - FOOTER_SIZE)
        footer = Footer.parse(f.read(FOOTER_SIZE))
    if footer.payload_size != size - FOOTER_SIZE:
        raise PackageError(f"{path}: the payload size doesn't match the file")
    return footer


def verify(
    path: Path, keys: dict[str, bytes], allow_unsigned: bool = False
) -> str | None:
    """The name of the key that signed @p path, or None (unsigned and
    allowed). Raises PackageError like the device would refuse it."""
    footer = read_footer(path)
    signed_by = None
    if footer.signed:
        matches = [n for n, k in keys.items() if key_id(k) == footer.key_id]
        if not matches:
            raise PackageError(
                f"{path}: signed by an unknown key ({footer.key_id.hex()})"
            )
        if not ed25519.verify(
            keys[matches[0]], footer.signed_message(), footer.signature
        ):
            raise PackageError(f"{path}: bad signature")
        signed_by = matches[0]
    elif not allow_unsigned:
        raise PackageError(f"{path}: not signed")
    sha = hashlib.sha512()
    with open(path, "rb") as f:
        left = footer.payload_size
        while left:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                raise PackageError(f"{path}: truncated")
            sha.update(chunk)
            left -= len(chunk)
    if sha.digest() != footer.digest:
        raise PackageError(f"{path}: damaged (SHA-512 doesn't match)")
    return signed_by


def extract(path: Path, dest: Path) -> dict:
    """Unpack @p path's three files into @p dest; returns config.json."""
    footer = read_footer(path)
    with open(path, "rb") as f:
        payload = f.read(footer.payload_size)
    try:
        data = lzma.LZMADecompressor(format=lzma.FORMAT_XZ).decompress(payload)
    except lzma.LZMAError as e:
        raise PackageError(f"{path}: the payload is not xz ({e})") from e
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(
        fileobj=io.BytesIO(data), mode="r:", format=tarfile.USTAR_FORMAT
    ) as tar:
        members = tar.getmembers()
        if [m.name for m in members] != list(MEMBERS) or not all(
            m.isreg() for m in members
        ):
            raise PackageError(
                f"{path}: holds {[m.name for m in members]}, not {list(MEMBERS)}"
            )
        for m in members:
            (dest / m.name).write_bytes(tar.extractfile(m).read())
    return json.loads((dest / MEMBERS[0]).read_text())


def container_lxc_config(name: str, text: str) -> str:
    """An old lxc.container.conf for the package of @p name: its root file
    system where containerd mounts it now (/var/lib/aember/containers/<name>;
    some boards used another directory name before)."""
    lines = []
    for line in text.splitlines():
        if line.split("=", 1)[0].strip() == "lxc.rootfs.path":
            line = f"lxc.rootfs.path = dir:/var/lib/aember/containers/{name}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def container_config(name: str, entry: dict) -> dict:
    """A container's config.json from an old containers.json entry (the
    format before packages: paths, args and settings per board)."""
    config: dict = {"name": name, "command": entry.get("container", {}).get("args", [])}
    for key in CONTAINER_SETTINGS:
        if key in entry:
            config[key] = entry[key]
    return config


def install(
    package: Path, root: Path, keys: dict[str, bytes], allow_unsigned: bool
) -> dict:
    """
    Install @p package (<name>.aember) like a device does, into
    @p root/<name>/: checked against @p keys, its three files and
    package.json. For images built with their packages installed already
    (`aember app pack`); returns package.json's content.
    """
    name = package.name[: -len(EXTENSION)] if package.name.endswith(EXTENSION) else ""
    if not name:
        raise PackageError(f"{package}: not a {EXTENSION} file")
    signed_by = verify(package, keys, allow_unsigned)
    dest = root / name
    if dest.exists():
        raise PackageError(f"{dest} exists already")
    config = extract(package, dest)
    if config.get("name") != name:
        raise PackageError(f"{package}: config.json's \"name\" is not '{name}'")
    installed = {
        "digest": read_footer(package).digest.hex(),
        "name": name,
        "signed_by": signed_by,
    }
    (dest / "package.json").write_text(json.dumps(installed, indent=2) + "\n")
    return installed
