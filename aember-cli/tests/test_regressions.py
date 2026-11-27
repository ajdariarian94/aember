"""Regression tests for CLI bugs that had none: image building, the
compile_commands merge, `aember qemu build`, stale build directories."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from aember.cli.qemu import build as qemu_build
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.steps.clean.clean import CleanStep
from aember.steps.cmake import configure
from aember.steps.packaging import rootfs
from aember.utils.merge_compile_command import CompileCommandsMerger


class GuestfishTest(unittest.TestCase):
    def run_with(self, returncode, stderr):
        done = subprocess.CompletedProcess([], returncode, stderr=stderr.encode())
        image = mock.patch.object(
            rootfs, "get_running_container_image", return_value="img"
        )
        run = mock.patch.object(rootfs.subprocess, "run", return_value=done)
        with image, run, mock.patch.object(rootfs.sys, "stderr"):
            rootfs.run_guestfish(["exit"], what="test.img")

    # Regression: guestfish exited 0 after "No space left on device" and a
    # cut-off image shipped; any libguestfs error fails the pack now.
    def test_regression_libguestfs_error_with_exit_0_fails(self):
        with self.assertRaises(RuntimeError):
            self.run_with(0, "libguestfs: error: write: No space left on device\n")

    def test_clean_run_passes(self):
        self.run_with(0, "")

    def test_nonzero_exit_fails(self):
        with self.assertRaises(RuntimeError):
            self.run_with(1, "")

    # Regression: the Debug + tests rootfs didn't fit the fixed 512 MB image;
    # the image is sized from what goes in.
    def test_regression_image_grows_with_its_contents(self):
        with tempfile.TemporaryDirectory() as t:
            tree = Path(t) / "tree"
            tree.mkdir()
            ctx = Context()
            self.assertEqual(rootfs.rootfs_size_mb(ctx, tree), ctx.rootfs_size)
            with open(tree / "big", "wb") as f:
                f.truncate(600 * 1024 * 1024)  # sparse: no real disk used
            self.assertGreater(rootfs.rootfs_size_mb(ctx, tree), 600)


class CompileCommandsTest(unittest.TestCase):
    # Regression: each build merged the merged file back in and appended
    # another copy of every entry.
    def test_regression_merge_skips_its_output_and_dedupes(self):
        with tempfile.TemporaryDirectory() as t:
            ctx = Context(
                root=Path(t), architecture="x64-poky-linux", build_type="Debug"
            )
            build = Path(t) / "build/configure" / ctx.architecture / ctx.variant
            sub = build / "libs"
            sub.mkdir(parents=True)
            entry = {"directory": "/b", "file": "a.cpp", "command": "c++ a.cpp"}
            (sub / "compile_commands.json").write_text(json.dumps([entry]))
            for _ in range(3):
                out = CompileCommandsMerger.merge(ctx)
            self.assertEqual(json.loads(out.read_text()), [entry])


class QemuBuildTest(unittest.TestCase):
    def steps(self, **kwargs):
        with mock.patch.object(qemu_build, "Pipeline") as pipeline:
            qemu_build.build_qemu(
                architecture=Architecture.x64_linux,
                build_type=kwargs.get("build_type", BuildType.release),
                build_tests=kwargs.get("build_tests", False),
            )
        return pipeline.call_args.args[0]

    # Regression: the clean condition read `build_tests or BuildType.debug`
    # (always true), so every build started by cleaning.
    def test_regression_cleans_only_with_tests(self):
        self.assertFalse(any(isinstance(s, CleanStep) for s in self.steps()))
        debug = self.steps(build_type=BuildType.debug)
        self.assertFalse(any(isinstance(s, CleanStep) for s in debug))
        with_tests = self.steps(build_tests=True)
        self.assertTrue(any(isinstance(s, CleanStep) for s in with_tests))


def write_probe(directory: Path, *sysroots: str) -> Path:
    """A CMakeFiles/<v>/CMakeCXXCompiler.cmake listing @p sysroots' usr/include."""
    sdk = "/aember_ws/aember/build/aember-resources/aarch64-poky-linux/sdk/sysroots"
    dirs = [f"{sdk}/x86_64-pokysdk-linux/usr/aarch64-poky-linux/include"]
    dirs += [f"{sdk}/{name}/usr/include" for name in sysroots]
    probe = directory / "CMakeFiles" / "4.1.0" / "CMakeCXXCompiler.cmake"
    probe.parent.mkdir(parents=True)
    probe.write_text(
        'set(CMAKE_CXX_COMPILER_ID "GNU")\n'
        f'set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "{";".join(dirs)}")\n'
    )
    return probe


class StaleBuildDirTest(unittest.TestCase):
    # Regression: build/configure/aarch64-poky-linux/ was configured with an
    # older SDK whose sysroot was cortexa72-poky-linux; the current SDK's is
    # cortexa53. The cached probe no longer counted the sysroot's usr/include
    # as implicit, FindLibLZMA's -isystem <sysroot>/usr/include broke
    # libstdc++'s #include_next, and every aarch64 build of utils/package
    # failed with "stdlib.h: No such file or directory" (2026-11-26).
    def test_probe_of_another_sysroot_is_stale(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            probe = write_probe(
                build / "aember-libs-prefix" / "src" / "aember-libs-build",
                "cortexa72-poky-linux",
            )
            self.assertEqual(
                configure.stale_probe(build, "cortexa53-poky-linux"), probe
            )
            self.assertEqual(
                configure.probe_sysroots(probe),
                {"x86_64-pokysdk-linux", "cortexa72-poky-linux"},
            )

    def test_probe_of_the_current_sysroot_is_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_probe(Path(tmp), "cortexa53-poky-linux")
            self.assertIsNone(configure.stale_probe(Path(tmp), "cortexa53-poky-linux"))
            self.assertIsNone(configure.stale_probe(Path(tmp) / "missing", "x"))

    def test_configure_removes_a_stale_build_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ctx = Context(architecture="aarch64-poky-linux", build_type="Release")
            ctx.root = root
            build = root / "build" / "configure" / "aarch64-poky-linux" / "Release"
            write_probe(build, "cortexa72-poky-linux")
            (build / "CMakeCache.txt").write_text("old\n")
            configure.ConfigureStep().drop_stale_build_dir(ctx)
            self.assertFalse(build.exists())
            # a current one stays
            write_probe(build, "cortexa53-poky-linux")
            configure.ConfigureStep().drop_stale_build_dir(ctx)
            self.assertTrue(build.exists())


if __name__ == "__main__":
    unittest.main()
