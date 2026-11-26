"""Opt-in packages: only what a board's packages.json lists is fetched and
packed, links are recreated, and services.json can't start a program the
image doesn't have."""

import json
import tempfile
import unittest
from pathlib import Path

from aember.config.resources import package_dir
from aember.core.context import Context
from aember.steps.packaging.pack import PackStep
from aember.steps.packaging.packages import stage_packages
from aember.steps.resources.fetch_resources import FetchResources
from aember.utils.packages import listed_packages

ARCH = "x64-poky-linux"


class _Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.overlay = self.root / "resources" / ARCH / "rootfs"
        self.etc = self.overlay / "etc" / "aember"
        self.etc.mkdir(parents=True)
        self.dest = self.root / "package" / "packages_root"

    def tearDown(self):
        self._tmp.cleanup()

    def list_packages(self, *names):
        (self.etc / "packages.json").write_text(json.dumps({"packages": list(names)}))

    def install_package(self, name, files, links=(), dirs=()):
        pkg = package_dir(self.root, ARCH, name)
        for rel, data in files.items():
            f = pkg / "rootfs" / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(data)
            f.chmod(0o755)
        (pkg / "symlinks.txt").write_text("".join(f"{p}\t{t}\n" for p, t in links))
        (pkg / "packages.txt").write_text(f"{name} core2_64 1.0\n")
        (pkg / "dirs.txt").write_text("".join(f"{p}\t{m}\n" for p, m in dirs))

    def install_dropbear(self):
        self.install_package(
            "dropbear",
            {
                "usr/sbin/dropbearmulti": b"\x7fELF dropbear",
                "usr/lib/libz.so.1.3.1": b"\x7fELF libz",
            },
            links=[
                ("/usr/sbin/dropbear", "./dropbearmulti"),
                ("/usr/bin/ssh", "/usr/sbin/dropbearmulti"),
                ("/usr/lib/libz.so.1", "libz.so.1.3.1"),
                ("/lib/firmware/a b.txt", "c d.txt"),
            ],
            dirs=[("/etc/dropbear", "0700")],
        )

    def stage(self):
        return stage_packages(self.root, ARCH, self.overlay, self.dest)


class ListedPackagesTest(_Fixture):
    def test_no_file_lists_nothing(self):
        self.assertEqual(listed_packages(self.overlay), [])

    def test_order_kept_and_duplicates_dropped(self):
        self.list_packages("dropbear", "wifi", "dropbear")
        self.assertEqual(listed_packages(self.overlay), ["dropbear", "wifi"])

    def test_malformed_raises(self):
        for text in ("{ nope", '{"packages": "dropbear"}', '{"packages": [""]}', "{}"):
            (self.etc / "packages.json").write_text(text)
            with self.subTest(text=text), self.assertRaises(ValueError):
                listed_packages(self.overlay)


class StagePackagesTest(_Fixture):
    def test_listed_package_with_links(self):
        self.install_dropbear()
        self.list_packages("dropbear")
        self.stage()
        multi = self.dest / "usr/sbin/dropbearmulti"
        self.assertEqual(multi.read_bytes(), b"\x7fELF dropbear")
        self.assertTrue(multi.stat().st_mode & 0o100)
        self.assertEqual(
            (self.dest / "usr/sbin/dropbear").readlink(), Path("./dropbearmulti")
        )
        self.assertEqual(
            (self.dest / "usr/bin/ssh").readlink(), Path("/usr/sbin/dropbearmulti")
        )
        self.assertEqual(
            (self.dest / "usr/lib/libz.so.1").read_bytes(), b"\x7fELF libz"
        )
        # Names with spaces (firmware): the lists are tab-separated.
        self.assertEqual(
            (self.dest / "lib/firmware/a b.txt").readlink(), Path("c d.txt")
        )
        keys = self.dest / "etc/dropbear"
        self.assertTrue(keys.is_dir())
        self.assertEqual(keys.stat().st_mode & 0o777, 0o700)

    def test_unlisted_package_is_not_staged(self):
        self.install_dropbear()
        self.list_packages()
        self.stage()
        self.assertFalse((self.dest / "usr").exists())

    def test_users_are_always_written(self):
        self.stage()
        self.assertTrue(
            (self.dest / "etc/passwd").read_text().startswith("root:x:0:0:")
        )
        self.assertEqual((self.dest / "etc/group").read_text(), "root:x:0:\n")
        self.assertTrue((self.dest / "root").is_dir())

    def test_package_users_are_added_once(self):
        self.install_package("connman", {"usr/sbin/connmand": b"\x7fELF"})
        pkg = package_dir(self.root, ARCH, "connman")
        user = "messagebus:x:997:997::/var/lib/dbus:/bin/false"
        (pkg / "users.txt").write_text(user + "\nroot:x:0:0:other:/:/bin/false\n")
        (pkg / "groups.txt").write_text("messagebus:x:997:\n")
        self.list_packages("connman")
        self.stage()
        passwd = (self.dest / "etc/passwd").read_text().splitlines()
        self.assertEqual(passwd, ["root:x:0:0:root:/root:/bin/sh", user])
        group = (self.dest / "etc/group").read_text().splitlines()
        self.assertEqual(group, ["root:x:0:", "messagebus:x:997:"])

    def test_listed_but_not_installed_fails(self):
        self.list_packages("wifi")
        with self.assertRaisesRegex(FileNotFoundError, "wifi"):
            self.stage()

    def test_restaging_starts_clean(self):
        self.install_dropbear()
        self.list_packages("dropbear")
        self.stage()
        self.list_packages()
        self.stage()
        self.assertFalse((self.dest / "usr/sbin/dropbearmulti").exists())

    def test_staging_does_not_change_the_package(self):
        self.install_dropbear()
        self.list_packages("dropbear")
        self.stage()
        (self.dest / "usr/sbin/dropbear").unlink()
        src = package_dir(self.root, ARCH, "dropbear") / "rootfs"
        self.assertEqual(
            sorted(p.name for p in (src / "usr/sbin").iterdir()), ["dropbearmulti"]
        )


class FetchFilterTest(_Fixture):
    ASSETS = [
        {"component": c, "architecture": ARCH}
        for c in ("kernel", "busybox", "lxc", "sdk", "dropbear", "wifi")
    ]

    def wanted(self):
        ctx = Context(architecture=ARCH, build_type="Release", root=self.root)
        return sorted(a["component"] for a in FetchResources().wanted(ctx, self.ASSETS))

    def test_base_components_only_without_packages(self):
        self.assertEqual(self.wanted(), ["busybox", "kernel", "lxc", "sdk"])

    def test_listed_package_is_fetched(self):
        self.list_packages("dropbear")
        self.assertEqual(self.wanted(), ["busybox", "dropbear", "kernel", "lxc", "sdk"])

    def test_listed_package_not_released_fails(self):
        self.list_packages("connman")
        with self.assertRaisesRegex(RuntimeError, "connman"):
            self.wanted()


class ServiceExecutablesTest(_Fixture):
    def check(self, *executables):
        names = [Path(e).name for e in executables]
        (self.etc / "services.json").write_text(
            json.dumps({"services": [{"name": n} for n in names]})
        )
        for name, exe in zip(names, executables):
            folder = self.overlay / "usr/lib/aember/services" / name
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "config.json").write_text(json.dumps({"executable": exe}))
        ctx = Context(architecture=ARCH, build_type="Release", root=self.root)
        PackStep().check_service_executables(ctx, [self.dest, self.overlay])

    def test_packaged_executable_passes(self):
        self.install_dropbear()
        self.list_packages("dropbear")
        self.stage()
        self.check("/usr/sbin/dropbear")

    def test_executable_without_package_fails(self):
        self.stage()
        with self.assertRaisesRegex(FileNotFoundError, "packages.json"):
            self.check("/usr/sbin/dropbear")

    def test_a_service_without_its_folder_fails(self):
        self.stage()
        (self.etc / "services.json").write_text(
            json.dumps({"services": [{"name": "nowhere"}]})
        )
        ctx = Context(architecture=ARCH, build_type="Release", root=self.root)
        with self.assertRaisesRegex(FileNotFoundError, "nowhere"):
            PackStep().check_service_executables(ctx, [self.dest, self.overlay])


if __name__ == "__main__":
    unittest.main()
