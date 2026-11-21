"""Only the containers a board's containers.json lists go into its image,
and only their images are fetched."""

import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from aember.steps.packaging.overlay import stage_overlay
from aember.utils import check_resources as cr
from aember.utils.containers import container_files, missing_images

ARCH = "x64-poky-linux"


def entry(name, **extra):
    return {
        "name": name,
        "type": "container",
        "container": {
            "squashfs": f"/var/lib/aember/{name}/{name}.squashfs",
            "rootfs": f"/var/lib/aember/containers/{name}",
            "config_path": f"/var/lib/aember/{name}/lxc.container.conf",
            **extra,
        },
    }


class _Fixture(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.overlay = self.root / "resources" / ARCH / "rootfs"
        self.etc = self.overlay / "etc" / "aember"
        self.etc.mkdir(parents=True)
        (self.etc / "services.json").write_text("{}")
        self.dest = self.root / "package" / "overlay_root"

    def tearDown(self):
        self._tmp.cleanup()

    def add_container(self, name, image=True):
        d = self.overlay / "var" / "lib" / "aember" / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "lxc.container.conf").write_text(f"# {name}\n")
        if image:
            (d / f"{name}.squashfs").write_bytes(b"hsqs" + name.encode())

    def list_containers(self, *names):
        (self.etc / "containers.json").write_text(
            json.dumps({"containers": [entry(n) for n in names]})
        )

    def staged(self):
        stage_overlay(self.overlay, self.dest)
        return sorted(p.name for p in (self.dest / "var/lib/aember").iterdir())


class OverlayTest(_Fixture):
    def test_only_listed_containers_are_staged(self):
        for name in ("aember-stats", "aember-monitor", "alpine"):
            self.add_container(name)
        self.list_containers("aember-stats")
        self.assertEqual(self.staged(), ["aember-stats"])
        image = self.dest / "var/lib/aember/aember-stats/aember-stats.squashfs"
        self.assertEqual(image.read_bytes(), b"hsqsaember-stats")
        self.assertTrue((self.dest / "etc/aember/containers.json").is_file())
        self.assertTrue((self.dest / "etc/aember/services.json").is_file())

    def test_no_containers_json_stages_no_container(self):
        self.add_container("alpine")
        self.assertEqual(self.staged(), [])

    def test_other_files_are_kept(self):
        self.add_container("alpine")
        other = self.overlay / "var" / "lib" / "aember" / "state"
        other.mkdir(parents=True)
        (other / "keep").write_text("x")
        (self.overlay / "etc" / "hostname").write_text("aember\n")
        self.list_containers()
        self.assertEqual(self.staged(), ["state"])
        self.assertTrue((self.dest / "etc/hostname").is_file())

    def test_listed_container_without_image_fails(self):
        self.add_container("aember-monitor", image=False)
        self.list_containers("aember-monitor")
        with self.assertRaisesRegex(FileNotFoundError, "aember-monitor.squashfs"):
            stage_overlay(self.overlay, self.dest)

    def test_malformed_containers_json_fails(self):
        (self.etc / "containers.json").write_text("{ not json")
        with self.assertRaises(ValueError):
            stage_overlay(self.overlay, self.dest)

    def test_restaging_starts_clean(self):
        for name in ("aember-stats", "alpine"):
            self.add_container(name)
        self.list_containers("aember-stats", "alpine")
        self.assertEqual(self.staged(), ["aember-stats", "alpine"])
        self.list_containers("alpine")
        self.assertEqual(self.staged(), ["alpine"])

    def test_staging_does_not_change_the_overlay(self):
        self.add_container("alpine")
        self.add_container("aember-stats")
        self.list_containers("aember-stats")
        self.staged()
        self.assertTrue(
            (self.overlay / "var/lib/aember/alpine/alpine.squashfs").is_file()
        )

    def test_container_files_and_missing_images(self):
        self.add_container("aember-stats")
        self.add_container("aember-monitor", image=False)
        self.list_containers("aember-stats", "aember-monitor")
        self.assertEqual(len(container_files(self.overlay)), 4)
        self.assertEqual(
            [p.name for p in missing_images(self.overlay)],
            ["aember-monitor.squashfs"],
        )


class CheckResourcesTest(_Fixture):
    """check_resources() fetches the release tarball only when a listed image
    is missing, and never overwrites existing (git-tracked) files."""

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

    def test_nothing_fetched_when_listed_images_exist(self):
        self.add_container("aember-stats")
        self.list_containers("aember-stats")
        self.assertEqual(self.run_check(), [])

    def test_unlisted_missing_image_is_not_fetched(self):
        self.add_container("aember-stats")
        self.add_container("alpine", image=False)
        self.list_containers("aember-stats")
        self.assertEqual(self.run_check(), [])

    def test_one_missing_listed_image_fetches_and_keeps_configs(self):
        self.add_container("aember-stats")
        self.add_container("aember-monitor", image=False)
        self.list_containers("aember-stats", "aember-monitor")
        tarball = self.make_tarball(
            {
                "var/lib/aember/aember-monitor/aember-monitor.squashfs": b"new",
                "var/lib/aember/aember-monitor/lxc.container.conf": b"old conf",
                "var/lib/aember/aember-stats/aember-stats.squashfs": b"old",
            }
        )
        downloads = self.run_check(tarball)
        self.assertEqual(len(downloads), 1)
        self.assertIn("v9.9.9/aember-resources-v9.9.9.tar.gz", downloads[0])
        d = self.overlay / "var/lib/aember"
        self.assertEqual(
            (d / "aember-monitor/aember-monitor.squashfs").read_bytes(), b"new"
        )
        self.assertEqual(
            (d / "aember-monitor/lxc.container.conf").read_text(), "# aember-monitor\n"
        )
        self.assertEqual(
            (d / "aember-stats/aember-stats.squashfs").read_bytes(), b"hsqsaember-stats"
        )

    def test_image_not_in_release_only_warns(self):
        self.add_container("brand-new", image=False)
        self.list_containers("brand-new")
        with self.assertLogs("aember.check_resources", level="WARNING") as logs:
            self.run_check(self.make_tarball({}))
        self.assertIn("brand-new.squashfs", "\n".join(logs.output))


if __name__ == "__main__":
    unittest.main()
