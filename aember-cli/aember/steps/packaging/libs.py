import logging
import re
import shutil
import subprocess
from pathlib import Path

from aember.config.libs import ARCH_CONFIG
from aember.config.resources import lxc_dir, sdk_dir
from aember.config.sdk import SDK_SYSROOT_MAP

log = logging.getLogger("aember.pack.libs")


# ---------------------------------------------------------------------------
# ELF dependency resolution
# ---------------------------------------------------------------------------


def get_dynamic_dependencies(binary_path: Path) -> list[str]:
    """
    'NEEDED' libraries of an ELF file.

    Uses readelf, which reads ELF files of every architecture; the host's
    objdump only understands x86 and would silently find nothing for aarch64.
    """
    result = subprocess.run(
        ["readelf", "-d", str(binary_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"readelf -d {binary_path} failed: {result.stderr}")
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", result.stdout)


def get_interpreter(binary_path: Path) -> str:
    """The ELF program interpreter (PT_INTERP), e.g. /lib/ld-linux-x86-64.so.2."""
    result = subprocess.run(
        ["readelf", "-l", str(binary_path)],
        capture_output=True,
        text=True,
        check=True,
    )
    match = re.search(r"Requesting program interpreter: (\S+)\]", result.stdout)
    if not match:
        raise RuntimeError(f"No program interpreter in {binary_path}")
    return match.group(1)


def resolve_library_path(lib_name: str, search_paths: list[Path]) -> Path:
    for path in search_paths:
        found = path / lib_name
        if found.exists():
            return found
    raise FileNotFoundError(f"Could not find library: {lib_name}")


def collect_all_libs(
    start_binaries: list[Path],
    search_paths: list[Path],
) -> set[Path]:
    to_check = list(start_binaries)
    resolved_paths: set[Path] = set()
    found_names: set[str] = set()

    while to_check:
        current = to_check.pop(0)
        for dep_name in get_dynamic_dependencies(current):
            if dep_name in found_names:
                continue
            try:
                lib_path = resolve_library_path(dep_name, search_paths)
                resolved_paths.add(lib_path)
                found_names.add(dep_name)
                log.debug("resolved: %s -> %s", dep_name, lib_path)
                to_check.append(lib_path)
            except FileNotFoundError:
                pass  # kernel-provided (vdso etc.) — skip silently

    return resolved_paths


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def copy_runtime_libs(
    architecture: str,
    root: Path,
    initramfs_root: Path,
    build_type: str,
) -> None:
    cfg = ARCH_CONFIG.get(architecture)
    if not cfg:
        raise ValueError(
            f"Dynamic resolution not configured for '{architecture}'. "
            f"Known: {list(ARCH_CONFIG)}"
        )

    poky_sysroot = (
        sdk_dir(root, architecture) / "sysroots" / SDK_SYSROOT_MAP[architecture]
    )
    virt_libs = lxc_dir(root, architecture) / "libs"
    loader_name = cfg["loader_name"]

    aember_bin = (
        root / "build" / "install" / architecture / build_type / "bin" / "aember"
    )

    # ---------------------------------------------------------------------------
    # Search order:
    # 1. LXC libs (aember-resources lxc asset)
    # 2. Poky SDK target sysroot (glibc, libstdc++, ...)
    # ---------------------------------------------------------------------------
    search_paths: list[Path] = [
        virt_libs,
        poky_sysroot / "lib",
        poky_sysroot / "usr/lib",
        poky_sysroot / "usr/local/lib",
    ]

    log.info("Resolving dependencies for: %s", aember_bin)
    all_libs = collect_all_libs([aember_bin], search_paths)

    # Add the dynamic loader.
    loader_path = poky_sysroot / "lib" / loader_name
    if not loader_path.exists():
        loader_path = poky_sysroot / "lib64" / loader_name
    if not loader_path.exists():
        raise FileNotFoundError(
            f"Could not find loader '{loader_name}' in {poky_sysroot}"
        )
    all_libs.add(loader_path)

    # The loader goes where the binary asks for it (PT_INTERP): Poky puts it
    # in /lib on x86-64 and aarch64, unlike host toolchains (/lib64).
    interpreter = get_interpreter(aember_bin)
    if Path(interpreter).name != loader_name:
        raise RuntimeError(
            f"{aember_bin} wants interpreter {interpreter}, expected {loader_name}"
        )
    loader_dir = Path(interpreter).parent.relative_to("/")
    log.info("Program interpreter: %s", interpreter)

    log.info("Copying %d libraries to %s", len(all_libs), initramfs_root)

    for lib in sorted(all_libs, key=lambda p: p.name):
        dst_dir = initramfs_root / (loader_dir if lib.name == loader_name else "lib")
        dst_dir.mkdir(parents=True, exist_ok=True)

        if lib.is_symlink():
            real_lib = lib.resolve()

            dst_real = dst_dir / real_lib.name
            if not dst_real.exists():
                shutil.copy2(real_lib, dst_dir)
                log.debug("copy:    %s", real_lib.name)

            dst_link = dst_dir / lib.name
            dst_link.unlink(missing_ok=True)
            dst_link.symlink_to(real_lib.name)
            log.debug("symlink: %s -> %s", lib.name, real_lib.name)
        else:
            shutil.copy2(lib, dst_dir)
            log.debug("copy:    %s", lib.name)
