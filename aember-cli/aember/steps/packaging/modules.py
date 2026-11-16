import json
import logging
import shutil
import subprocess
from pathlib import Path

from aember.config.resources import kernel_dir

log = logging.getLogger("aember.pack")


def _modules_json_path(root: Path, architecture: str) -> Path:
    """Path to modules.json in the rootfs overlay for this architecture."""
    return (
        root / "resources" / architecture / "rootfs" / "etc" / "aember" / "modules.json"
    )


def _load_modules(root: Path, architecture: str) -> list[str]:
    """
    Load module list from resources/<architecture>/rootfs/etc/aember/modules.json.
    Falls back to empty list if file not found.
    """
    path = _modules_json_path(root, architecture)
    if not path.exists():
        log.warning("modules.json not found at %s — no modules will be packed", path)
        return []

    cfg = json.loads(path.read_text())
    modules = cfg.get("modules", [])
    log.info("Loaded %d module(s) from %s", len(modules), path)
    return modules


def _find_kernel_version(modules_lib_root: Path) -> str | None:
    modules_dir = modules_lib_root / "modules"
    if not modules_dir.exists():
        return None
    versions = [d.name for d in modules_dir.iterdir() if d.is_dir()]
    return versions[0] if len(versions) == 1 else None


def _find_module_file(version_dir: Path, module_name: str) -> Path | None:
    normalized = module_name.replace("-", "_")
    dashed = module_name.replace("_", "-")

    for candidate in [
        f"{normalized}.ko",
        f"{dashed}.ko",
        f"{normalized}.ko.gz",
        f"{dashed}.ko.gz",
        f"{normalized}.ko.xz",
        f"{dashed}.ko.xz",
        f"{normalized}.ko.zst",
        f"{dashed}.ko.zst",
    ]:
        matches = list(version_dir.rglob(candidate))
        if matches:
            return matches[0]

    return None


# Kernel metadata copied into the rootfs so modprobe/depmod know which
# modules are built in (modprobe of a built-in module then succeeds).
MODULE_METADATA = ("modules.builtin", "modules.builtin.modinfo", "modules.order")


def _builtin_modules(version_dir: Path) -> set[str]:
    """Names of modules built into the kernel (from modules.builtin)."""
    path = version_dir / "modules.builtin"
    if not path.exists():
        return set()
    return {
        Path(line.strip()).name.removesuffix(".ko").replace("-", "_")
        for line in path.read_text().splitlines()
        if line.strip()
    }


def _module_deps(version_dir: Path) -> dict[str, list[str]]:
    """
    Module dependencies of the kernel: module rel path → rel paths it needs.

    Read from the modules.dep that the aember-resources kernel asset ships
    (since v0.1.4); it used to be regenerated here with `depmod -n`.
    """
    path = version_dir / "modules.dep"
    if not path.exists():
        raise FileNotFoundError(
            f"{path} not found: the kernel asset ships modules.dep since "
            f"aember-resources v0.1.4. Fetch a current kernel "
            f"(RESOURCES_VERSION in config/resources.py)."
        )
    text = path.read_text()

    deps: dict[str, list[str]] = {}
    for line in text.splitlines():
        mod, sep, rest = line.partition(":")
        if sep and mod.strip().endswith((".ko", ".ko.gz", ".ko.xz", ".ko.zst")):
            deps[mod.strip()] = rest.split()
    return deps


def _with_deps(rels: list[str], deps: dict[str, list[str]]) -> list[str]:
    """rels plus everything they (transitively) depend on, dependencies first."""
    ordered: list[str] = []

    def visit(rel: str) -> None:
        if rel in ordered:
            return
        for dep in deps.get(rel, []):
            visit(dep)
        ordered.append(rel)

    for rel in rels:
        visit(rel)
    return ordered


def _decompress_module(ko_path: Path) -> Path:
    suffixes = "".join(ko_path.suffixes)

    if suffixes.endswith(".ko.zst"):
        cmd = ["zstd", "-d", str(ko_path), "--rm"]
    elif suffixes.endswith(".ko.gz"):
        cmd = ["gunzip", str(ko_path)]
    elif suffixes.endswith(".ko.xz"):
        cmd = ["unxz", str(ko_path)]
    else:
        return ko_path

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Decompression failed for {ko_path.name}: {result.stderr}")
    return ko_path.with_suffix("")


def _run_depmod(rootfs_root: Path, kernel_version: str) -> None:
    log.info("Running depmod for %s", kernel_version)
    result = subprocess.run(
        ["depmod", "-b", str(rootfs_root), kernel_version],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        log.warning("depmod failed: %s", result.stderr)
    else:
        log.info("depmod completed successfully")


def copy_kernel_modules(
    architecture: str,
    root: Path,
    rootfs_root: Path,
    build_type: str,
    extra_modules: list[str] | None = None,
) -> None:
    """
    Copy kernel modules into the rootfs at /lib/modules/<kernel_version>/.

    Module list is loaded dynamically from:
      resources/<architecture>/rootfs/etc/aember/modules.json

    Additional modules can be passed via extra_modules (e.g. debug-only ones).

    @param architecture   Build system architecture e.g. "aarch64-poky-linux".
    @param root           Project root (ctx.root).
    @param rootfs_root    Root of the target rootfs being assembled.
    @param build_type     "Debug" or "Release".
    @param extra_modules  Optional additional modules to pack on top of modules.json.
    """
    modules_lib_root = kernel_dir(root, architecture) / "lib"

    if not modules_lib_root.exists():
        raise FileNotFoundError(
            f"Kernel modules lib not found at {modules_lib_root}. "
            f"Run `aember qemu build` to fetch the kernel from aember-resources."
        )

    kernel_version = _find_kernel_version(modules_lib_root)
    if not kernel_version:
        raise RuntimeError(f"Could not detect kernel version under {modules_lib_root}.")

    log.info("Kernel version: %s", kernel_version)

    # ---------------------------------------------------------------------------
    # Load module list from modules.json — single source of truth
    # ---------------------------------------------------------------------------
    module_names = _load_modules(root, architecture)

    if extra_modules:
        module_names.extend(m for m in extra_modules if m not in module_names)
        log.info("Added %d extra module(s): %s", len(extra_modules), extra_modules)

    # Deduplicate preserving order.
    unique_modules = list(dict.fromkeys(module_names))

    log.info("Packing %d module(s): %s", len(unique_modules), ", ".join(unique_modules))

    # ---------------------------------------------------------------------------
    # Copy modules
    # ---------------------------------------------------------------------------
    src_version_dir = modules_lib_root / "modules" / kernel_version
    dst_version_dir = rootfs_root / "lib" / "modules" / kernel_version
    dst_version_dir.mkdir(parents=True, exist_ok=True)

    for meta in MODULE_METADATA:
        if (src_version_dir / meta).exists():
            shutil.copy2(src_version_dir / meta, dst_version_dir / meta)

    builtin = _builtin_modules(src_version_dir)

    missing: list[str] = []
    copied: list[str] = []

    requested: list[str] = []
    for mod_name in unique_modules:
        ko_path = _find_module_file(src_version_dir, mod_name)

        if ko_path is None and mod_name.replace("-", "_") in builtin:
            log.info("Built in: %s", mod_name)
            continue

        if ko_path is None:
            log.warning("Module '%s' not found under %s", mod_name, src_version_dir)
            missing.append(mod_name)
            continue

        requested.append(ko_path.relative_to(src_version_dir).as_posix())

    # Pack what the requested modules depend on too (kernel modules.dep), so
    # modprobe can resolve them in the rootfs.
    for rel_str in _with_deps(requested, _module_deps(src_version_dir)):
        if rel_str not in requested:
            log.info("Dependency: %s", rel_str)
        ko_path = src_version_dir / rel_str
        rel = Path(rel_str)
        dst_file = dst_version_dir / rel
        dst_file.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ko_path, dst_file)

        if dst_file.suffix in (".zst", ".gz", ".xz"):
            try:
                dst_file = _decompress_module(dst_file)
                log.info("Packed + decompressed: %s", rel)
            except RuntimeError as e:
                log.warning("%s", e)
        else:
            log.info("Packed: %s", rel)

        copied.append(str(rel))

    log.info("Done: %d packed, %d missing", len(copied), len(missing))

    if missing:
        raise RuntimeError(
            f"Missing kernel modules for '{architecture}': {missing}. "
            f"Enable them (=m or =y) in the aember-resources kernel config."
        )

    _run_depmod(rootfs_root, kernel_version)
