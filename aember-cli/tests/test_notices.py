"""Third-party notices on the image (steps/packaging/notices.py) and the
inventory behind them (doc/licensing/components.json)."""

import gzip
import json
import tempfile
import unittest
from pathlib import Path

from aember.steps.packaging.notices import (
    COMPONENTS_JSON,
    NOTICES_PATH,
    TEXTS_DIR,
    installed_packages,
    load_components,
    present,
    write_notices,
)

REPO = Path(__file__).resolve().parents[2]


def ids(components):
    return {c.id for c in components}


class InventoryTest(unittest.TestCase):
    def test_the_repository_inventory_loads(self):
        components = load_components(REPO)
        self.assertGreater(len(components), 30)
        for c in components:
            self.assertTrue(c.license and c.obligation and c.where, c.id)
            # Only our own code goes without a licence text.
            if not c.license.startswith("LicenseRef-"):
                self.assertTrue(c.texts, c.id)

    def test_every_text_is_used(self):
        used = {t for c in load_components(REPO) for t in c.texts}
        files = {p.stem for p in (REPO / TEXTS_DIR).glob("*.txt")}
        self.assertEqual(files - used, {"Apache-2.0"})  # mbedTLS, phase 2

    def test_wolfssl_in_our_containers_is_marked_as_a_blocker(self):
        c = {c.id: c for c in load_components(REPO)}["wolfssl-auth"]
        self.assertIn("BLOCKER", c.obligation)


class CheckedLoadTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / TEXTS_DIR).mkdir(parents=True)
        (self.root / TEXTS_DIR / "MIT.txt").write_text("MIT text")

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, *components):
        (self.root / COMPONENTS_JSON).write_text(
            json.dumps({"components": list(components)})
        )

    def comp(self, **extra):
        c = {
            "id": "x",
            "name": "X",
            "version": "1",
            "license": "MIT",
            "where": "rootfs",
            "linking": "dynamic",
            "obligation": "notice",
            "url": "https://x",
            "texts": ["MIT"],
        }
        c.update(extra)
        return c

    def test_unknown_key_is_an_error(self):
        self.write(self.comp(licence="MIT"))  # typo of "license"
        with self.assertRaises(ValueError):
            load_components(self.root)

    def test_missing_text_is_an_error(self):
        self.write(self.comp(texts=["GPL-2.0-only"]))
        with self.assertRaises(ValueError):
            load_components(self.root)

    def test_duplicate_id_is_an_error(self):
        self.write(self.comp(), self.comp())
        with self.assertRaises(ValueError):
            load_components(self.root)


class PresenceTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.components = load_components(REPO)

    def tearDown(self):
        self._tmp.cleanup()

    def tree(self, name, *files):
        t = self.tmp / name
        for f in files:
            (t / f).parent.mkdir(parents=True, exist_ok=True)
            (t / f).write_bytes(b"x")
        return t

    def test_an_empty_image_has_only_the_always_present(self):
        found = ids(present(self.components, [self.tmp / "none"], set()))
        self.assertEqual(found, {"linux"})

    def test_files_in_any_tree(self):
        trees = [
            self.tree("initramfs", "bin/busybox", "usr/bin/aember", "lib/libc.so.6"),
            self.tree("services", "lib/liblxc.so.1"),
        ]
        found = ids(present(self.components, trees, set()))
        for want in ("busybox", "aember", "glibc", "lxc", "nlohmann-json", "spdlog"):
            self.assertIn(want, found)
        self.assertNotIn("musl", found)
        self.assertNotIn("qt", found)
        self.assertNotIn("googletest", found)  # no test binaries

    def test_containers_bring_what_is_inside_them(self):
        trees = [
            self.tree(
                "overlay",
                "var/lib/aember/aember-monitor/aember-monitor.squashfs",
                "var/lib/aember/aember-auth/aember-auth.squashfs",
            )
        ]
        found = ids(present(self.components, trees, set()))
        self.assertTrue({"qt", "freetype", "monocypher", "wolfssl-auth"} <= found)
        self.assertNotIn("alpine-demo", found)

    def test_packages_by_yocto_name(self):
        pkg = self.tmp / "connman"
        pkg.mkdir()
        (pkg / "packages.txt").write_text(
            "connman core2_64 1.44\nlibreadline8 core2_64 8.3\n\n"
        )
        names = installed_packages([pkg, self.tmp / "missing"])
        self.assertEqual(names, {"connman", "libreadline8"})
        found = ids(present(self.components, [], names))
        self.assertTrue({"connman", "readline", "linux"} <= found)
        self.assertNotIn("dropbear", found)


class WriteTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_notices_file(self):
        image = self.tmp / "image"
        (image / "bin").mkdir(parents=True)
        (image / "bin" / "busybox").write_bytes(b"x")
        (image / "usr" / "bin").mkdir(parents=True)
        (image / "usr" / "bin" / "aember").write_bytes(b"x")
        share = self.tmp / "vcpkg" / "share"
        (share / "spdlog").mkdir(parents=True)
        (share / "spdlog" / "copyright").write_text("Copyright (c) 2016 Gabi Melman.")
        out = write_notices(REPO, [image], [], self.tmp / "notices", share)
        self.assertEqual(out, self.tmp / "notices" / NOTICES_PATH)
        text = gzip.decompress(out.read_bytes()).decode()
        self.assertIn("BusyBox 1.37.0", text)
        self.assertIn("Linux kernel 6.16", text)
        self.assertIn("Gabi Melman", text)  # the upstream copyright file
        self.assertIn("written offer", text)  # SOURCE-OFFER.md
        # Each licence text once, though kernel and BusyBox share it.
        self.assertEqual(text.count("=== GPL-2.0-only ==="), 1)
        self.assertNotIn("=== LGPL-3.0-only ===", text)  # no Qt here
        # Reproducible: the same input gives the same bytes.
        again = write_notices(REPO, [image], [], self.tmp / "again", share)
        self.assertEqual(out.read_bytes(), again.read_bytes())

    def test_without_vcpkg_dir(self):
        out = write_notices(REPO, [], [], self.tmp / "n", None)
        self.assertTrue(out.is_file())


if __name__ == "__main__":
    unittest.main()
