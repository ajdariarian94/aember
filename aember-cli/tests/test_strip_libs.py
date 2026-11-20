"""Release images strip the shared libraries they pack (liblxc ships with
debug info from aember-resources)."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from aember.config.resources import sdk_dir
from aember.steps.packaging.libs import sdk_strip_tool, strip_libs

ARCH = "x64-poky-linux"


@unittest.skipUnless(shutil.which("gcc") and shutil.which("strip"), "needs gcc")
class StripLibsTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        bin_dir = (
            sdk_dir(self.root, ARCH)
            / "sysroots/x86_64-pokysdk-linux/usr/bin/x86_64-poky-linux"
        )
        bin_dir.mkdir(parents=True)
        (bin_dir / "x86_64-poky-linux-strip").symlink_to(shutil.which("strip"))
        self.lib = self.root / "lib"
        self.lib.mkdir()

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_finds_the_sdk_cross_strip(self):
        self.assertEqual(
            sdk_strip_tool(self.root, ARCH).name, "x86_64-poky-linux-strip"
        )

    def test_strips_debug_info_and_keeps_symlinks(self):
        src = self.root / "a.c"
        src.write_text("int answer(void) { return 42; }\n" * 1)
        so = self.lib / "liba.so.1.0"
        subprocess.run(
            ["gcc", "-g3", "-O0", "-shared", "-fPIC", "-o", str(so), str(src)],
            check=True,
        )
        (self.lib / "liba.so.1").symlink_to("liba.so.1.0")
        before = so.stat().st_size

        saved = strip_libs(self.root, ARCH, self.lib)

        self.assertGreater(saved, 0)
        self.assertEqual(so.stat().st_size, before - saved)
        self.assertTrue((self.lib / "liba.so.1").is_symlink())
        sections = subprocess.run(
            ["readelf", "-S", str(so)], capture_output=True, text=True
        ).stdout
        self.assertNotIn(".debug_info", sections)
        self.assertIn(".dynsym", sections)  # still loadable

    def test_empty_dir_is_fine(self):
        self.assertEqual(strip_libs(self.root, ARCH, self.lib), 0)


if __name__ == "__main__":
    unittest.main()
