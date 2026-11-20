"""`aember test` variants: coverage report, sanitizer/valgrind/root runs and
the instrumented toolchain flags."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from aember.core.context import Context
from aember.steps.testing import coverage
from aember.steps.testing.run_tests import SANITIZER_ENV, RunTests, tests_dir

ROOT = Path(__file__).resolve().parents[2]


class VariantTest(unittest.TestCase):
    def test_plain_and_instrumented_builds_never_share_a_directory(self):
        self.assertEqual(Context(build_type="Debug").variant, "Debug")
        ctx = Context(build_type="Debug", instrument="asan")
        self.assertEqual(ctx.variant, "Debug-asan")
        self.assertEqual(
            tests_dir(Path("/r"), ctx.variant),
            Path("/r/build/install/x64-poky-linux/Debug-asan/bin/tests"),
        )


class ComponentTest(unittest.TestCase):
    def test_sources_map_to_their_library_or_service(self):
        cases = {
            "libraries/utils/ipc/src/hub.cpp": "utils/ipc",
            "libraries/utils/ipc/include/aember-libs/utils/ipc/hub.h": "utils/ipc",
            "libraries/aember-init/src/aember-init.cpp": "aember-init",
            "services/netd/src/dhcp-client.cpp": "netd",
            "executables/aember/main.cpp": "aember",
        }
        for path, name in cases.items():
            with self.subTest(path=path):
                self.assertEqual(coverage.component(path), name)

    def test_tests_and_foreign_files_are_not_counted(self):
        for path in (
            "libraries/utils/ipc/tests/src/hub.cpp",
            "services/netd/tests/src/dhcp.cpp",
            "services/tests-common/daemon-runner.h",
            "build/vcpkg_installed/x/include/gtest.h",
            "main.cpp",
        ):
            with self.subTest(path=path):
                self.assertIsNone(coverage.component(path))

    def test_summarize_adds_up_per_component_and_total(self):
        files = [
            {
                "filename": "libraries/utils/ipc/src/a.cpp",
                "line_total": 10,
                "line_covered": 5,
                "function_total": 2,
                "function_covered": 1,
                "branch_total": 4,
                "branch_covered": 4,
            },
            {
                "filename": "libraries/utils/ipc/src/b.cpp",
                "line_total": 10,
                "line_covered": 10,
                "function_total": 2,
                "function_covered": 2,
                "branch_total": 0,
                "branch_covered": 0,
            },
            {
                "filename": "services/netd/src/c.cpp",
                "line_total": 20,
                "line_covered": 0,
                "function_total": 1,
                "function_covered": 0,
                "branch_total": 0,
                "branch_covered": 0,
            },
            {"filename": "libraries/utils/ipc/tests/x.cpp", "line_total": 99},
        ]
        comps = coverage.summarize(files)
        self.assertEqual(list(comps), ["netd", "utils/ipc"])
        ipc = comps["utils/ipc"].as_dict()
        self.assertEqual((ipc["lines"], ipc["line_percent"]), (20, 75.0))
        self.assertEqual(ipc["function_percent"], 75.0)
        self.assertEqual(ipc["branch_percent"], 100.0)
        self.assertIsNone(comps["netd"].as_dict()["branch_percent"])
        all_ = coverage.total(comps).as_dict()
        self.assertEqual((all_["lines"], all_["lines_covered"]), (40, 15))

    def test_markdown_table_for_ci(self):
        comps = coverage.summarize(
            [
                {
                    "filename": "services/netd/src/c.cpp",
                    "line_total": 4,
                    "line_covered": 3,
                    "function_total": 1,
                    "function_covered": 1,
                }
            ]
        )
        md = coverage.markdown(comps)
        self.assertIn("| netd | 75.0% | 100.0% | - | 4 |", md)
        self.assertIn("| **total** |", md)

    def test_gcovr_uses_the_sdk_gcov_and_skips_tests(self):
        cmd = coverage.gcovr_command(Path("/r"), Path("/r/build/coverage"))
        gcov = cmd[cmd.index("--gcov-executable") + 1]
        self.assertTrue(gcov.endswith("x86_64-poky-linux-gcov"))
        self.assertIn(r".*/tests/.*", cmd)
        self.assertIn("--exclude-throw-branches", cmd)
        self.assertIn("--merge-lines", cmd)
        self.assertEqual(cmd[-1], "/r/build/configure/x64-poky-linux/Debug-coverage")


class RunTestsCommandTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        sysroot = (
            self.root
            / "build/aember-resources/x64-poky-linux/sdk/sysroots/core2-64-poky-linux"
        )
        (sysroot / "lib").mkdir(parents=True)
        (sysroot / "lib" / "ld-linux-x86-64.so.2").touch()

    def tearDown(self):
        self.tmp.cleanup()

    def test_plain_runs_through_the_sdk_loader(self):
        cmd = RunTests()._command(self.root, Path("/t/x-unittest"))
        self.assertTrue(cmd[0].endswith("ld-linux-x86-64.so.2"))
        self.assertEqual(cmd[-1], "/t/x-unittest")

    def test_valgrind_wraps_the_loader(self):
        cmd = RunTests(valgrind=True)._command(self.root, Path("/t/x-unittest"))
        self.assertEqual(cmd[0], "valgrind")
        self.assertIn("--error-exitcode=99", cmd)
        self.assertTrue(any(c.startswith("--suppressions=") for c in cmd))

    def test_root_keeps_the_sanitizer_options(self):
        cmd = RunTests(as_root=True)._command(self.root, Path("/t/x-unittest"))
        self.assertEqual(cmd[:3], ["sudo", "-n", "env"])
        self.assertIn(f"ASAN_OPTIONS={SANITIZER_ENV['ASAN_OPTIONS']}", cmd)

    def test_valgrind_counts_only_the_test_process_leaks(self):
        from aember.steps.testing.run_tests import valgrind_leaked

        out = (
            "==100== Memcheck, a memory error detector\n"
            "==101==    definitely lost: 8,192 bytes in 1 blocks\n"  # a child
            "==100==    definitely lost: 0 bytes in 0 blocks\n"
        )
        self.assertEqual(valgrind_leaked(out), 0)
        self.assertEqual(
            valgrind_leaked(out + "==100==    definitely lost: 1,024 bytes\n"), 1024
        )
        self.assertEqual(valgrind_leaked("no valgrind here"), 0)

    def test_sanitizers_stop_at_the_first_error(self):
        for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS"):
            self.assertIn("halt_on_error=1", SANITIZER_ENV[key])

    def test_tsan_runs_without_aslr_so_it_does_not_reexec(self):
        r = RunTests()
        r.no_aslr = True
        cmd = r._command(self.root, Path("/t/x-unittest"))
        self.assertEqual(cmd[:3], ["setarch", "x86_64", "-R"])
        self.assertTrue(cmd[3].endswith("ld-linux-x86-64.so.2"))
        self.assertNotIn("setarch", RunTests()._command(self.root, Path("/t/x")))

    def test_lsan_suppressions_only_hide_the_dynamic_loader(self):
        from aember.steps.testing.run_tests import test_env

        repo = Path(__file__).resolve().parents[2]
        lsan = test_env(repo)["LSAN_OPTIONS"]
        supp = Path(lsan.split("suppressions=")[1].split(":")[0])
        rules = [
            line
            for line in supp.read_text().splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(rules, ["leak:ld-linux-x86-64.so.2"])


@unittest.skipUnless(shutil.which("cmake"), "needs cmake")
class InstrumentCmakeTest(unittest.TestCase):
    """tools/toolchains/instrument.cmake, run as a script."""

    def flags(self, instrument: str) -> subprocess.CompletedProcess:
        script = (
            f'set(AEMBER_INSTRUMENT "{instrument}")\n'
            f'include("{ROOT}/tools/toolchains/instrument.cmake")\n'
            'message(STATUS "CXX=${CMAKE_CXX_FLAGS_INIT}")\n'
            'message(STATUS "LD=${CMAKE_EXE_LINKER_FLAGS_INIT}")\n'
        )
        with tempfile.NamedTemporaryFile("w", suffix=".cmake") as f:
            f.write(script)
            f.flush()
            return subprocess.run(
                ["cmake", "-P", f.name], capture_output=True, text=True
            )

    def test_each_variant_sets_its_flags(self):
        expected = {
            "": ("", ""),
            "coverage": ("--coverage", "--coverage"),
            "asan": ("-fsanitize=address,undefined", "-fsanitize=address,undefined"),
            "tsan": ("-fsanitize=thread", "-fsanitize=thread"),
        }
        for instrument, (cxx, ld) in expected.items():
            with self.subTest(instrument=instrument):
                out = self.flags(instrument)
                self.assertEqual(out.returncode, 0, out.stderr)
                if cxx:
                    self.assertIn(cxx, out.stdout)
                    self.assertIn(f"LD= {ld}", out.stdout)
                else:
                    self.assertIn("CXX=\n", out.stdout)

    def test_an_unknown_variant_fails(self):
        out = self.flags("msan")
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("expected coverage, asan or tsan", out.stderr)


if __name__ == "__main__":
    unittest.main()
