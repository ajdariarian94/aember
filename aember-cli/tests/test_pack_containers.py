"""Only the containers a board's containers.json lists go into its image,
installed from their .aember packages, and only their packages are fetched
(made from the release's old layout when the release has no package)."""

import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from aember.steps.packaging.overlay import stage_overlay
from aember.utils import aember_package as pkg
from aember.utils import check_resources as cr
from aember.utils import ed25519
from aember.utils.containers import container_files, missing_images

ARCH = "x64-poky-linux"
SEED = bytes(range(32))


class _Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.overlay = self.root / "resources" / ARCH / "rootfs"
        self.etc = self.overlay / "etc" / "aember"
        self.etc.mkdir(parents=True)
        (self.etc / "services.json").write_text("{}")
        self.dest = self.root / "package" / "overlay_root"
        self.image = self.root / "image.squashfs"
        self.image.write_bytes(b"hsqs-image")

    def tearDown(self):
        self._tmp.cleanup()

    def add_package(self, name, seed=None):
        out = self.overlay / "var/lib/aember/packages" / f"{name}.aember"
        out.parent.mkdir(parents=True, exist_ok=True)
        pkg.write_package(
            out,
            {"name": name, "command": [f"/{name}"]},
            f"# {name}\n",
            self.image,
            seed,
        )
        return out

    def list_containers(self, *names, allow_unsigned=True):
        (self.etc / "containers.json").write_text(
            json.dumps(
                {
                    "allow_unsigned": allow_unsigned,
                    "containers": [{"name": n} for n in names],
                }
            )
        )

    def staged(self):
        stage_overlay(self.overlay, self.dest)
        return sorted(p.name for p in (self.dest / "var/lib/aember").iterdir())


class OverlayTest(_Fixture):
    def test_only_listed_containers_are_installed(self):
        for name in ("aember-stats", "aember-monitor", "alpine"):
            self.add_package(name)
        self.list_containers("aember-stats")
        # packages/ stays (empty): where a device's updates arrive.
        self.assertEqual(self.staged(), ["aember-stats", "packages"])
        d = self.dest / "var/lib/aember/aember-stats"
        self.assertEqual((d / "image.squashfs").read_bytes(), b"hsqs-image")
        self.assertEqual((d / "lxc.container.conf").read_text(), "# aember-stats\n")
        installed = json.loads((d / "package.json").read_text())
        self.assertEqual(installed["name"], "aember-stats")
        self.assertIsNone(installed["signed_by"])
        self.assertEqual(list((self.dest / "var/lib/aember/packages").iterdir()), [])
        self.assertTrue((self.dest / "etc/aember/services.json").is_file())

    def test_signed_packages_against_the_boards_keys(self):
        self.add_package("app", seed=SEED)
        keys = self.etc / "keys"
        keys.mkdir()
        (keys / "board.pub").write_text(pkg.public_key_text(ed25519.public_key(SEED)))
        self.list_containers("app", allow_unsigned=False)
        self.staged()
        installed = json.loads(
            (self.dest / "var/lib/aember/app/package.json").read_text()
        )
        self.assertEqual(installed["signed_by"], "board")

    def test_unsigned_package_on_a_board_that_wants_signed_ones_fails(self):
        self.add_package("app")
        self.list_containers("app", allow_unsigned=False)
        with self.assertRaises(pkg.PackageError):
            stage_overlay(self.overlay, self.dest)

    def test_no_containers_json_installs_nothing(self):
        self.add_package("alpine")
        self.assertEqual(self.staged(), ["packages"])

    def test_listed_container_without_package_fails(self):
        self.list_containers("aember-monitor")
        with self.assertRaisesRegex(FileNotFoundError, "aember-monitor.aember"):
            stage_overlay(self.overlay, self.dest)

    def test_malformed_or_old_containers_json_fails(self):
        (self.etc / "containers.json").write_text("{ not json")
        with self.assertRaises(ValueError):
            stage_overlay(self.overlay, self.dest)
        (self.etc / "containers.json").write_text(
            json.dumps({"containers": [{"name": "a", "container": {}}]})
        )
        with self.assertRaisesRegex(ValueError, "before .aember packages"):
            stage_overlay(self.overlay, self.dest)

    def test_old_layout_leftovers_are_left_out(self):
        old = self.overlay / "var/lib/aember/alpine"
        old.mkdir(parents=True)
        (old / "alpine.squashfs").write_bytes(b"old")
        state = self.overlay / "var/lib/aember/state"
        state.mkdir()
        (state / "keep").write_text("x")
        self.list_containers()
        with self.assertLogs("aember.pack.overlay", level="WARNING"):
            self.assertEqual(self.staged(), ["packages", "state"])

    def test_restaging_starts_clean(self):
        for name in ("aember-stats", "alpine"):
            self.add_package(name)
        self.list_containers("aember-stats", "alpine")
        self.assertEqual(self.staged(), ["aember-stats", "alpine", "packages"])
        self.list_containers("alpine")
        self.assertEqual(self.staged(), ["alpine", "packages"])

    def test_container_files_and_missing_images(self):
        self.add_package("aember-stats")
        self.list_containers("aember-stats", "aember-monitor")
        self.assertEqual(len(container_files(self.overlay)), 2)
        self.assertEqual(
            [p.name for p in missing_images(self.overlay)], ["aember-monitor.aember"]
        )


class CheckResourcesTest(_Fixture):
    """check_resources() fetches the release tarball only when a listed
    package is missing, and takes only the missing packages out of it."""

    def make_tarball(self, files):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            for name, data in files.items():
                info = tarfile.TarInfo(f"resources/{ARCH}/rootfs/{name}")
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        return buf.getvalue()

    def run_check(self, tarball=b""):
        downloads = []

        def fake_download(url, dest):
            downloads.append(url)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(tarball)

        with mock.patch.object(cr, "_download", fake_download), mock.patch.object(
            cr, "get_version", return_value="9.9.9"
        ), mock.patch.object(cr, "_get_repo", return_value="owner/aember"):
            cr.check_resources(self.root)
        return downloads

    def test_nothing_fetched_when_listed_packages_exist(self):
        self.add_package("aember-stats")
        self.list_containers("aember-stats")
        self.assertEqual(self.run_check(), [])

    def test_a_package_in_the_release_is_taken(self):
        self.add_package("aember-stats")
        self.list_containers("aember-stats", "aember-monitor")
        made = self.add_package("aember-monitor")
        data = made.read_bytes()
        made.unlink()
        downloads = self.run_check(
            self.make_tarball({"var/lib/aember/packages/aember-monitor.aember": data})
        )
        self.assertEqual(len(downloads), 1)
        self.assertIn("v9.9.9/aember-resources-v9.9.9.tar.gz", downloads[0])
        self.assertEqual(made.read_bytes(), data)

    def test_a_release_of_the_old_layout_is_made_into_packages(self):
        self.list_containers("alpine-echo")
        old_json = {
            "containers": [
                {
                    "name": "alpine-echo",
                    "container": {
                        "squashfs": "/var/lib/aember/alpine/alpine.squashfs",
                        "rootfs": "/var/lib/aember/containers/alpine",
                        "config_path": "/var/lib/aember/alpine/lxc.container.conf",
                        "args": ["/bin/sh", "/echo.sh"],
                    },
                    "restart_policy": "always",
                    "max_restart_attempts": 5,
                }
            ]
        }
        self.run_check(
            self.make_tarball(
                {
                    "etc/aember/containers.json": json.dumps(old_json).encode(),
                    "var/lib/aember/alpine/alpine.squashfs": b"hsqs-alpine",
                    "var/lib/aember/alpine/lxc.container.conf": (
                        b"lxc.rootfs.path = /var/lib/aember/containers/alpine\n"
                        b"lxc.uts.name = alpine-echo\n"
                    ),
                }
            )
        )
        package = self.overlay / "var/lib/aember/packages/alpine-echo.aember"
        self.assertIsNone(pkg.verify(package, {}, allow_unsigned=True))
        out = self.root / "out"
        config = pkg.extract(package, out)
        self.assertEqual(
            config,
            {
                "name": "alpine-echo",
                "command": ["/bin/sh", "/echo.sh"],
                "restart_policy": "always",
                "max_restart_attempts": 5,
            },
        )
        self.assertEqual(
            (out / "lxc.container.conf").read_text(),
            "lxc.rootfs.path = dir:/var/lib/aember/containers/alpine-echo\n"
            "lxc.uts.name = alpine-echo\n",
        )
        self.assertEqual((out / "image.squashfs").read_bytes(), b"hsqs-alpine")
        # The release's own containers.json doesn't replace the board's.
        self.assertEqual(
            json.loads((self.etc / "containers.json").read_text())["containers"],
            [{"name": "alpine-echo"}],
        )

    def test_package_not_in_release_only_warns(self):
        self.list_containers("brand-new")
        with self.assertLogs("aember.check_resources", level="WARNING") as logs:
            self.run_check(self.make_tarball({}))
        self.assertIn("brand-new.aember", "\n".join(logs.output))


if __name__ == "__main__":
    unittest.main()
