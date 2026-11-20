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
    def test_musl_toolchain_uses_the_musl_sdk_and_bigger_thread_stacks(self):
        text = (ROOT / "tools/toolchains/x64-poky-linux-musl.cmake").read_text()
        self.assertIn("x86_64-poky-linux-musl-g++", text)
        self.assertIn("core2-64-poky-linux-musl", text)
        self.assertIn("-Wl,-z,stack-size=", text)
        self.assertNotRegex(text, r"x86_64-poky-linux[-/](?!musl)")

    def test_musl_loader(self):
        self.assertEqual(
            ARCH_CONFIG["x64-poky-linux-musl"]["loader_name"], "ld-musl-x86_64.so.1"
        )


if __name__ == "__main__":
    unittest.main()
