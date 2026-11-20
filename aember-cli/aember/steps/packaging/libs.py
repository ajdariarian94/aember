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
    dest_root: Path,
    binaries: list[Path],
    dlopen: list[str] | None = None,
    present_in: Path | None = None,
) -> None:
    """
    Copy what @p binaries need at run time (their NEEDED, recursively, plus
    the @p dlopen libraries and theirs) into dest_root/lib, and the dynamic
    loader where the binaries ask for it. With @p present_in (the initramfs
    tree, when packing services into the rootfs-only tree), libraries that
    tree already has are skipped, and so is the loader.
    """
    cfg = ARCH_CONFIG.get(architecture)
    if not cfg:
        raise ValueError(
            f"Dynamic resolution not configured for '{architecture}'. "
            f"Known: {list(ARCH_CONFIG)}"
        )
    if not binaries:
        return

    poky_sysroot = (
        sdk_dir(root, architecture) / "sysroots" / SDK_SYSROOT_MAP[architecture]
    )
    virt_libs = lxc_dir(root, architecture) / "libs"
    loader_name = cfg["loader_name"]

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

    # Libraries loaded with dlopen aren't in anyone's NEEDED (liblxc, which
    # aember-containerd loads): pack them and their own dependencies.
    dlopened = [resolve_library_path(name, search_paths) for name in dlopen or []]

    log.info(
        "Resolving dependencies for: %s (+ %s)",
        ", ".join(b.name for b in binaries),
        dlopen or "no dlopen libraries",
    )
    all_libs = collect_all_libs([*binaries, *dlopened], search_paths)
    all_libs.update(dlopened)

    # The loader goes where the binaries ask for it (PT_INTERP): Poky puts it
    # in /lib on x86-64 and aarch64, unlike host toolchains (/lib64).
    for binary in binaries:
        interpreter = get_interpreter(binary)
        if Path(interpreter).name != loader_name:
            raise RuntimeError(
                f"{binary} wants interpreter {interpreter}, expected {loader_name}"
            )
    loader_dir = Path(get_interpreter(binaries[0])).parent.relative_to("/")

    if present_in is None:
        loader_path = poky_sysroot / "lib" / loader_name
        if not loader_path.exists():
            loader_path = poky_sysroot / "lib64" / loader_name
        if not loader_path.exists():
            raise FileNotFoundError(
                f"Could not find loader '{loader_name}' in {poky_sysroot}"
            )
        all_libs.add(loader_path)
        log.info("Program interpreter: /%s/%s", loader_dir, loader_name)
    else:
        have = {
            p.name for d in ("lib", str(loader_dir)) for p in (present_in / d).glob("*")
        }
        all_libs = {lib for lib in all_libs if lib.name not in have}

    log.info("Copying %d libraries to %s", len(all_libs), dest_root)

    for lib in sorted(all_libs, key=lambda p: p.name):
        dst_dir = dest_root / (loader_dir if lib.name == loader_name else "lib")
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


def sdk_strip_tool(root: Path, architecture: str) -> Path:
    """The SDK's cross strip (<triple>-strip): the host's binutils only know
    x86-64."""
    bin_dir = sdk_dir(root, architecture) / "sysroots" / "x86_64-pokysdk-linux"
    tools = sorted((bin_dir / "usr" / "bin").glob("*/*-strip"))
    if not tools:
        raise FileNotFoundError(f"No cross strip in {bin_dir}/usr/bin/*/")
    return tools[0]


def strip_libs(root: Path, architecture: str, lib_dir: Path) -> int:
    """
    Strip debug info and local symbols from the shared libraries in
    @p lib_dir (Release images). liblxc from aember-resources keeps its
    debug info for Debug builds: 4.8 MB unstripped, 1.1 MB stripped (x64).
    Returns the bytes saved.
    """
    libs = [
        p for p in sorted(lib_dir.glob("*.so*")) if p.is_file() and not p.is_symlink()
    ]
    if not libs:
        return 0
    before = sum(p.stat().st_size for p in libs)
    subprocess.run(
        [str(sdk_strip_tool(root, architecture)), "--strip-unneeded"]
        + [str(p) for p in libs],
        check=True,
    )
    saved = before - sum(p.stat().st_size for p in libs)
    log.info(
        "Stripped %d libraries in %s: %d KB less", len(libs), lib_dir, saved // 1024
    )
    return saved
