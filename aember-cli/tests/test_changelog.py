"""`aember changelog`: version bump and changelog from a base ref."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from aember.core.context import Context
from aember.steps.changelog.changelog import ChangelogStep

CMAKE = "cmake_minimum_required(VERSION 3.19.5)\nproject(demo\n  VERSION {v}\n)\n"


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


class ChangelogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        git(self.root, "init", "-q", "-b", "main")
        (self.root / "CMakeLists.txt").write_text(CMAKE.format(v="0.12.0"))
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "release 0.12.0")
        git(self.root, "tag", "v0.12.0")
        (self.root / "feature.txt").write_text("x\n")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "a new feature")

    def tearDown(self):
        self.tmp.cleanup()

    def run_step(self, **data) -> list[Path]:
        ChangelogStep().run(Context(bump="minor", root=self.root, data=data))
        return sorted((self.root / "doc" / "changelog").glob("changes-*.md"))

    def test_on_main_without_since_finds_nothing(self):
        self.assertEqual(self.run_step(), [])
        self.assertIn("0.12.0", (self.root / "CMakeLists.txt").read_text())

    def test_since_the_last_tag_bumps_and_lists_the_changes(self):
        (changelog,) = self.run_step(since="v0.12.0")
        text = changelog.read_text()
        self.assertIn("`0.12.0` → `0.13.0`", text)
        self.assertIn("a new feature", text)
        self.assertIn("`feature.txt`", text)
        self.assertNotIn("release 0.12.0", text)
        self.assertIn("VERSION 0.13.0", (self.root / "CMakeLists.txt").read_text())


if __name__ == "__main__":
    unittest.main()
