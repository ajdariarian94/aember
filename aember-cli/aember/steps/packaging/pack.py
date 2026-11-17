import logging
import shutil
from pathlib import Path

from aember.config.resources import kernel_dir
from aember.core.context import Context
from aember.core.step import Step

from .busybox import install_busybox
from .initramfs import create_initramfs_tree, pack_initramfs
from .libs import copy_runtime_libs
from .modules import copy_kernel_modules
from .rootfs import create_rootfs_image

log = logging.getLogger("aember.pack")

# Maps build system architecture → kernel image filename
KERNEL_IMAGE_NAMES: dict[str, str] = {
    "x64-poky-linux": "bzImage",
    "aarch64-poky-linux": "Image",
    "arm-poky-linux": "zImage",
}


class PackStep(Step):

    # ---- Derived paths -------------------------------------------------

    def build_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "configure" / ctx.architecture / ctx.build_type

    def install_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "install" / ctx.architecture / ctx.build_type

    def toolchain_file(self, ctx: Context) -> Path:
        return ctx.root / "tools" / "toolchains" / f"{ctx.architecture}.cmake"

    def package_dir(self, ctx: Context) -> Path:
        return ctx.root / "build" / "package" / ctx.architecture / ctx.build_type

    def overlay_dir(self, ctx: Context) -> Path:
        return ctx.root / "resources" / ctx.architecture / "rootfs"

    def modules_json_path(self, ctx: Context) -> Path:
        return ctx.root / "resources" / ctx.architecture / "modules.json"

    def prepare_dirs(self, package_dir: Path, initramfs_root: Path) -> None:
        log.info("Preparing package directories")

        if package_dir.exists():
            log.debug("Removing existing package dir: %s", package_dir)
            shutil.rmtree(package_dir)

        package_dir.mkdir(parents=True)
        initramfs_root.mkdir(parents=True)

    # ---- File copy helpers ----------------------------------------------

    def _copy_tree_into(self, src: Path, dst: Path, label: str) -> None:
        """Copy all entries under src into dst, creating dst if needed."""
        if not src.exists():
            log.info("No %s found, skipping", label)
            return

        log.info("Copying %s", label)
        dst.mkdir(parents=True, exist_ok=True)

        for item in src.iterdir():
            dest = dst / item.name
            if item.is_dir():
                shutil.copytree(item, dest, dirs_exist_ok=True)
            else:
                shutil.copy2(item, dest)

    def copy_init(self, ctx: Context, initramfs_root: Path) -> None:
        log.info("Creating init symlink")
        (initramfs_root / "init").symlink_to("usr/bin/aember")

    def copy_aember(self, ctx: Context, initramfs_root: Path) -> None:
        log.info("Copying aember binary")

        install_dir = ctx.data.get("install_dir")
        if install_dir is None:
            raise RuntimeError("install_dir not found in context")

        aember_dst_dir = initramfs_root / "usr" / "bin"
        aember_dst_dir.mkdir(parents=True, exist_ok=True)

        aember_dst = aember_dst_dir / "aember"
        shutil.copy2(install_dir / "bin" / "aember", aember_dst)
        aember_dst.chmod(0o755)

    def copy_tests(self, ctx: Context, tests_root: Path) -> None:
        """Test binaries go into the rootfs image only (tests_root is copied
        in after the initramfs tree): in the initramfs they cost RAM at
        every boot, and 20 Debug suites (~360 MB) no longer fit in the VM's
        512 MB. They run from the debug shell, after the switch to the real
        root."""
        if tests_root.exists():
            shutil.rmtree(tests_root)
        if not ctx.build_tests:
            log.debug("build_tests disabled, skipping tests")
            return

        install_dir = ctx.data.get("install_dir")
        if install_dir is None:
            return

        self._copy_tree_into(
            install_dir / "bin" / "tests",
            tests_root / "usr" / "bin",
            "test binaries",
        )

    def copy_examples(self, ctx: Context, initramfs_root: Path) -> None:
        install_dir = ctx.data.get("install_dir")
        if install_dir is None:
            log.debug("install_dir not found, skipping examples")
            return

        self._copy_tree_into(
            install_dir / "examples",
            initramfs_root / "usr" / "bin",
            "examples",
        )

    def copy_kernel(self, ctx: Context, package_dir: Path) -> Path:
        log.info("Copying kernel image")

        kernel_name = KERNEL_IMAGE_NAMES.get(ctx.architecture)
        if not kernel_name:
            raise ValueError(
                f"No kernel image name for architecture '{ctx.architecture}'. "
                f"Known: {list(KERNEL_IMAGE_NAMES)}"
            )

        kernel_src = kernel_dir(ctx.root, ctx.architecture) / kernel_name
        kernel_dst = package_dir / "kernel"

        if not kernel_src.exists():
            raise FileNotFoundError(f"Kernel image not found: {kernel_src}")

        shutil.copy2(kernel_src, kernel_dst)
        return kernel_dst

    # ---- Main entry -------------------------------------------------------

    def run(self, ctx: Context) -> None:
        log.info("Packing artifacts (%s / %s)", ctx.architecture, ctx.build_type)

        build_dir = self.build_dir(ctx)
        build_dir.mkdir(parents=True, exist_ok=True)

        ctx.data["build_dir"] = build_dir
        ctx.data["install_dir"] = self.install_dir(ctx)

        package_dir = self.package_dir(ctx)
        initramfs_root = package_dir / "initramfs_root"

        self.prepare_dirs(package_dir, initramfs_root)

        log.info("Creating initramfs filesystem tree")
        create_initramfs_tree(initramfs_root)

        log.info("Installing busybox")
        install_busybox(ctx.architecture, ctx.root, initramfs_root)

        self.copy_aember(ctx, initramfs_root)
        tests_root = package_dir / "tests_root"
        self.copy_tests(ctx, tests_root)
        self.copy_examples(ctx, initramfs_root)
        self.copy_init(ctx, initramfs_root)

        log.info("Copying runtime libraries")
        copy_runtime_libs(ctx.architecture, ctx.root, initramfs_root, ctx.build_type)

        log.info("Copying kernel modules")
        self.modules_json_path(ctx)
        copy_kernel_modules(
            architecture=ctx.architecture,
            root=ctx.root,
            rootfs_root=initramfs_root,
            build_type=ctx.build_type,
        )

        log.info("Packing initramfs")
        initramfs = pack_initramfs(initramfs_root, package_dir)

        kernel = self.copy_kernel(ctx, package_dir)

        log.info("Creating root filesystem image")
        rootfs = create_rootfs_image(
            ctx=ctx,
            initramfs_root=initramfs_root,
            overlay_dir=self.overlay_dir(ctx),
            package_dir=package_dir,
            extra_trees=[tests_root],
        )

        ctx.data.update(
            {
                "package_dir": package_dir,
                "kernel": kernel,
                "initramfs": initramfs,
                "rootfs": rootfs,
            }
        )

        log.info("Pack step completed successfully")
