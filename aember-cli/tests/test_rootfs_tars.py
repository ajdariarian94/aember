"""Image trees go in as tars owned by root, in order, links kept."""

import tarfile
import tempfile
import unittest
from pathlib import Path

from aember.steps.packaging.rootfs import tar_in_commands


class TarInTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def tree(self, name, files, links=()):
        t = self.root / name
        for rel, text in files.items():
            (t / rel).parent.mkdir(parents=True, exist_ok=True)
            (t / rel).write_text(text)
        for rel, target in links:
            (t / rel).parent.mkdir(parents=True, exist_ok=True)
            (t / rel).symlink_to(target)
        return t

    def test_commands_in_order_and_root_owned(self):
        a = self.tree("initramfs_root", {"usr/bin/aember": "x"})
        b = self.tree(
            "overlay_root",
            {"etc/aember/services.json": "{}"},
            links=[("usr/bin/ssh", "/usr/sbin/dropbearmulti")],
        )
        missing = self.root / "nope"
        cmds = tar_in_commands([a, missing, b], self.root / "tars")
        self.assertEqual(len(cmds), 2)
        self.assertTrue(cmds[0].startswith("tar-in ") and cmds[0].endswith(" /"))
        self.assertIn("initramfs_root", cmds[0])
        self.assertIn("overlay_root", cmds[1])
        with tarfile.open(cmds[1].split()[1]) as tar:
            members = {m.name: m for m in tar.getmembers()}
        self.assertIn("etc/aember/services.json", members)
        self.assertTrue(all(m.uid == 0 and m.gid == 0 for m in members.values()))
        self.assertTrue(members["usr/bin/ssh"].issym())
        self.assertEqual(members["usr/bin/ssh"].linkname, "/usr/sbin/dropbearmulti")


if __name__ == "__main__":
    unittest.main()
