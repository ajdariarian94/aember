import json
import logging
import shutil
from pathlib import Path

from aember.config.libs import SERVICE_DLOPEN_LIBS
from aember.config.resources import kernel_dir, package_dir as resources_package_dir
from aember.core.context import Context
from aember.core.step import Step
from aember.steps.cmake.sdk_build import build_dir, install_dir
from aember.utils.packages import listed_packages
from aember.utils.services import SERVICES_DIR, service_config, service_names

from .busybox import install_busybox
from .initramfs import create_initramfs_tree, pack_initramfs
from .libs import copy_runtime_libs, strip_libs
from .modules import copy_kernel_modules
from .notices import write_notices
from .overlay import stage_overlay
from .packages import package_executables, stage_packages
from .rootfs import create_rootfs_image

log = logging.getLogger("aember.pack")

# Maps build system architecture → kernel image filename
KERNEL_IMAGE_NAMES: dict[str, str] = {
    "x64-poky-linux": "bzImage",
    "aarch64-poky-linux": "Image",
    "arm-poky-linux": "zImage",
    "riscv64-poky-linux": "Image",
    "x64-poky-linux-musl": "bzImage",
    "aarch64-poky-linux-musl": "Image",
    "arm-poky-linux-musl": "zImage",
    "riscv64-poky-linux-musl": "Image",
}


class PackStep(Step):

    # ---- Derived paths -------------------------------------------------

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

    def copy_services(self, ctx: Context, services_root: Path) -> list[Path]:
        """
        The services (built from services/) that this board's services.json
        starts, into services_root/usr/bin, each with its folder
        (usr/lib/aember/services/<name>/config.json, from
        services/<dir>/config.json; one the overlay brings itself stays the
        overlay's). Only the rootfs gets this tree: services run after the
        switch to the real root, so they cost no initramfs RAM, and a service
        nobody lists isn't packed at all.
        """
        if services_root.exists():
            shutil.rmtree(services_root)
        built = ctx.data["install_dir"] / "services"
        overlay = self.overlay_dir(ctx)
        try:
            entries = service_names(overlay)
        except ValueError as e:
            log.warning("%s: no services packed", e)
            return []

        packed: list[Path] = []
        for entry in entries:
            found = service_config(entry["name"], overlay, ctx.root)
            if found is None:
                continue  # check_service_executables reports it
            config, source = found
            if not source.is_relative_to(overlay):
                folder = services_root / SERVICES_DIR / entry["name"]
                folder.mkdir(parents=True, exist_ok=True)
                (folder / "config.json").write_text(json.dumps(config, indent=2) + "\n")
            exe = Path(str(config.get("executable", "")))
            src = built / exe.name
            if exe.parent != Path("/usr/bin") or not src.is_file():
                continue  # not one of ours (BusyBox, ConnMan, ...)
            dst = services_root / "usr" / "bin" / exe.name
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            dst.chmod(0o755)
            packed.append(dst)
        available = sorted(p.name for p in built.glob("*")) if built.is_dir() else []
        log.info(
            "Services packed: %s (built: %s)",
            ", ".join(p.name for p in packed) or "none",
            ", ".join(available) or "none",
        )
        return packed

    def check_service_executables(self, ctx: Context, trees: list[Path]) -> None:
        """Every service services.json starts must have its folder and its
        executable in the image: one that doesn't (an SSH server without its
        package) would never start, or restart forever, on the device."""
        overlay = self.overlay_dir(ctx)
        try:
            entries = service_names(overlay)
        except ValueError:
            return  # copy_services already warned
        missing = []
        for entry in entries:
            found = service_config(entry["name"], overlay, ctx.root)
            if found is None:
                missing.append(
                    f"{entry['name']} (no {SERVICES_DIR}/{entry['name']}/config.json)"
                )
                continue
            exe = str(found[0].get("executable", "")).lstrip("/")
            if exe and not any(
                (t / exe).exists() or (t / exe).is_symlink() for t in trees
            ):
                missing.append(f"{entry['name']} (/{exe})")
        if missing:
            raise FileNotFoundError(
                "services.json starts services the image doesn't have: "
                + ", ".join(missing)
                + ". A program from an opt-in package needs the package in "
                "etc/aember/packages.json; a service of the board's own needs "
                f"its folder in the overlay ({SERVICES_DIR}/<name>/)."
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

        build_dir(ctx).mkdir(parents=True, exist_ok=True)
        ctx.data["build_dir"] = build_dir(ctx)
        ctx.data["install_dir"] = install_dir(ctx)

        package_dir = self.package_dir(ctx)
        initramfs_root = package_dir / "initramfs_root"

        self.prepare_dirs(package_dir, initramfs_root)

        # Only the containers containers.json lists (also for the Pi/USB
        # images and the AppImage, which use this tree). First, so a missing
        # image fails the pack before anything is built.
        overlay_root = stage_overlay(
            self.overlay_dir(ctx), package_dir / "overlay_root"
        )
        # The opt-in packages packages.json lists (rootfs only).
        packages_root = stage_packages(
            ctx.root,
            ctx.architecture,
            self.overlay_dir(ctx),
            package_dir / "packages_root",
        )

        log.info("Creating initramfs filesystem tree")
        create_initramfs_tree(initramfs_root)

        log.info("Installing busybox")
        install_busybox(ctx.architecture, ctx.root, initramfs_root)

        self.copy_aember(ctx, initramfs_root)
        tests_root = package_dir / "tests_root"
        self.copy_tests(ctx, tests_root)
        self.copy_init(ctx, initramfs_root)

        log.info("Copying runtime libraries")
        copy_runtime_libs(
            ctx.architecture,
            ctx.root,
            initramfs_root,
            binaries=[initramfs_root / "usr" / "bin" / "aember"],
        )

        services_root = package_dir / "services_root"
        services = self.copy_services(ctx, services_root)
        copy_runtime_libs(
            ctx.architecture,
            ctx.root,
            services_root,
            binaries=services,
            dlopen=[
                lib for s in services for lib in SERVICE_DLOPEN_LIBS.get(s.name, [])
            ],
            present_in=initramfs_root,
        )
        # What the packages' programs need from the C library (and anything
        # else in the SDK) that the initramfs and services don't bring: a
        # package ships everything but the C library.
        copy_runtime_libs(
            ctx.architecture,
            ctx.root,
            packages_root,
            binaries=package_executables(packages_root),
            present_in=[initramfs_root, services_root],
            extra_search=[packages_root / "lib", packages_root / "usr" / "lib"],
        )
        if ctx.build_type == "Release":
            # Debug keeps the symbols (gdb); the SDK's glibc and libstdc++
            # come stripped already, aember-resources' liblxc doesn't.
            for tree in (initramfs_root, services_root, packages_root):
                strip_libs(ctx.root, ctx.architecture, tree / "lib")

        self.check_service_executables(
            ctx, [initramfs_root, services_root, packages_root, overlay_root]
        )

        log.info("Copying kernel modules")
        self.modules_json_path(ctx)
        copy_kernel_modules(
            architecture=ctx.architecture,
            root=ctx.root,
            rootfs_root=initramfs_root,
            build_type=ctx.build_type,
        )

        # Third-party notices of what this image contains (rootfs only):
        # /usr/share/licenses/aember/NOTICES.gz, doc/licensing/.
        notices_root = package_dir / "notices_root"
        if notices_root.exists():
            shutil.rmtree(notices_root)
        write_notices(
            ctx.root,
            trees=[
                initramfs_root,
                services_root,
                packages_root,
                overlay_root,
                tests_root,
            ],
            package_dirs=[
                resources_package_dir(ctx.root, ctx.architecture, name)
                for name in listed_packages(self.overlay_dir(ctx))
            ],
            out_root=notices_root,
            vcpkg_share=build_dir(ctx) / "vcpkg_installed" / ctx.architecture / "share",
        )

        log.info("Packing initramfs")
        initramfs = pack_initramfs(initramfs_root, package_dir)

        kernel = self.copy_kernel(ctx, package_dir)

        log.info("Creating root filesystem image")
        rootfs = create_rootfs_image(
            ctx=ctx,
            initramfs_root=initramfs_root,
            overlay_dir=overlay_root,
            package_dir=package_dir,
            extra_trees=[services_root, packages_root, tests_root, notices_root],
        )

        ctx.data.update(
            {
                "package_dir": package_dir,
                "kernel": kernel,
                "initramfs": initramfs,
                "rootfs": rootfs,
                "services_root": services_root,
                "overlay_root": overlay_root,
                "packages_root": packages_root,
            }
        )

        log.info("Pack step completed successfully")
