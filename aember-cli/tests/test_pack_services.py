"""`aember app pack` picks the services a board's services.json lists."""

import json
import tempfile
import unittest
from pathlib import Path

from aember.core.context import Context
from aember.steps.packaging.pack import PackStep

ARCH = "x64-poky-linux"


class PackServicesTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.install = self.root / "install"
        (self.install / "services").mkdir(parents=True)
        for name in ("aember-netd", "aember-containerd", "aember-extra"):
            (self.install / "services" / name).write_bytes(b"\x7fELF")
        self.etc = self.root / "resources" / ARCH / "rootfs" / "etc" / "aember"
        self.etc.mkdir(parents=True)
        self.ctx = Context(architecture=ARCH, build_type="Release", root=self.root)
        self.ctx.data["install_dir"] = self.install
        self.out = self.root / "services_root"

    def tearDown(self):
        self._tmp.cleanup()

    def write_services(self, executables):
        processes = [{"name": Path(e).name, "executable": e} for e in executables]
        (self.etc / "services.json").write_text(json.dumps({"processes": processes}))

    def packed(self):
        return sorted(p.name for p in PackStep().copy_services(self.ctx, self.out))

    def test_only_listed_services_are_packed(self):
        self.write_services(["/usr/bin/aember-netd", "/usr/bin/aember-child-demo"])
        self.assertEqual(self.packed(), ["aember-netd"])
        self.assertTrue((self.out / "usr/bin/aember-netd").is_file())
        self.assertFalse((self.out / "usr/bin/aember-containerd").exists())

    def test_without_netd_nothing_of_it_is_packed(self):
        self.write_services(["/usr/bin/aember-containerd", "/usr/sbin/connmand"])
        self.assertEqual(self.packed(), ["aember-containerd"])

    def test_executable_must_be_in_usr_bin(self):
        self.write_services(["/opt/aember-netd"])
        self.assertEqual(self.packed(), [])

    def test_repacking_starts_clean(self):
        self.write_services(["/usr/bin/aember-netd", "/usr/bin/aember-extra"])
        self.packed()
        self.write_services(["/usr/bin/aember-netd"])
        self.assertEqual(self.packed(), ["aember-netd"])
        self.assertFalse((self.out / "usr/bin/aember-extra").exists())

    def test_unreadable_services_json_packs_nothing(self):
        (self.etc / "services.json").write_text("{ not json")
        self.assertEqual(self.packed(), [])
        (self.etc / "services.json").unlink()
        self.assertEqual(self.packed(), [])


if __name__ == "__main__":
    unittest.main()
