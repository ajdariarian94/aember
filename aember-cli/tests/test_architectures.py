"""Every architecture is complete: CLI maps, toolchain, triplet, overlay."""

import json
import unittest
from pathlib import Path

from aember.config.architecture import Architecture
from aember.config.libs import ARCH_CONFIG, BASE_MODULES, DEBUG_ONLY_MODULES
from aember.config.qemu import QEMU_RUNNER_MAP
from aember.config.sdk import SDK_ENV_MAP, SDK_SYSROOT_MAP, VCPKG_TRIPLET_MAP
from aember.steps.launching.chroot import _BINFMT, NATIVE_ARCHS
from aember.steps.launching.launch import GDB_PROFILES, QEMU_PROFILES
from aember.steps.packaging.pack import KERNEL_IMAGE_NAMES

ROOT = Path(__file__).resolve().parents[2]


class ArchitectureTest(unittest.TestCase):
    def test_every_map_knows_every_architecture(self):
        maps = {
            "ARCH_CONFIG": ARCH_CONFIG,
            "BASE_MODULES": BASE_MODULES,
            "DEBUG_ONLY_MODULES": DEBUG_ONLY_MODULES,
            "QEMU_RUNNER_MAP": QEMU_RUNNER_MAP,
            "SDK_ENV_MAP": SDK_ENV_MAP,
            "SDK_SYSROOT_MAP": SDK_SYSROOT_MAP,
            "VCPKG_TRIPLET_MAP": VCPKG_TRIPLET_MAP,
            "QEMU_PROFILES": QEMU_PROFILES,
            "GDB_PROFILES": GDB_PROFILES,
            "KERNEL_IMAGE_NAMES": KERNEL_IMAGE_NAMES,
        }
        for arch in Architecture:
            for name, m in maps.items():
                with self.subTest(arch=arch.value, map=name):
                    self.assertIn(arch.value, m)
            if arch.value not in NATIVE_ARCHS:
                with self.subTest(arch=arch.value, map="_BINFMT"):
                    self.assertIn(arch.value, _BINFMT)

    def test_toolchain_triplet_and_overlay_exist(self):
        for arch in Architecture:
            a = arch.value
            with self.subTest(arch=a):
                toolchain = ROOT / "tools" / "toolchains" / f"{a}.cmake"
                self.assertTrue(toolchain.is_file())
                self.assertIn(SDK_SYSROOT_MAP[a], toolchain.read_text())
                triplet = VCPKG_TRIPLET_MAP[a]
                self.assertTrue(
                    (ROOT / "tools" / "triplets" / f"{triplet}.cmake").is_file()
                    or (
                        ROOT / "vcpkg" / "triplets" / "community" / f"{triplet}.cmake"
                    ).is_file()
                    or not (ROOT / "vcpkg" / "triplets").is_dir(),
                )
                etc = ROOT / "resources" / a / "rootfs" / "etc" / "aember"
                for f in ("services.json", "network.json", "modules.json"):
                    json.loads((etc / f).read_text())


class MuslVariantTest(unittest.TestCase):
    MUSL = [a.value for a in Architecture if a.value.endswith("-musl")]

    def test_there_are_musl_variants_of_glibc_targets(self):
        self.assertIn("x64-poky-linux-musl", self.MUSL)
        for arch in self.MUSL:
            self.assertIn(arch.removesuffix("-musl"), [a.value for a in Architecture])

    def test_musl_toolchains_use_the_musl_sdk_and_bigger_thread_stacks(self):
        for arch in self.MUSL:
            with self.subTest(arch=arch):
                text = (ROOT / f"tools/toolchains/{arch}.cmake").read_text()
                self.assertIn(f'build/aember-resources/{arch}"', text)
                self.assertIn(SDK_SYSROOT_MAP[arch], text)
                self.assertIn("-Wl,-z,stack-size=", text)
                # every compiler/tool path points into the musl toolchain
                for line in text.splitlines():
                    if "/usr/bin/" in line and "POKY_SDK_ROOT" in line:
                        self.assertIn("musl", line)

    def test_musl_loaders(self):
        for arch in self.MUSL:
            with self.subTest(arch=arch):
                self.assertTrue(ARCH_CONFIG[arch]["loader_name"].startswith("ld-musl-"))


if __name__ == "__main__":
    unittest.main()
