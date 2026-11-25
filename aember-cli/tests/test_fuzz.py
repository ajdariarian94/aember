"""`aember fuzz`: targets, commands, results (no fuzzing run here)."""

import unittest
from pathlib import Path

from aember.core.context import Context
from aember.steps.testing import fuzz

REPO = Path(__file__).resolve().parents[2]
ROOT = Path("/ws")
CTX = Context(architecture="x64-poky-linux", build_type="Debug", root=ROOT)


class TargetsTest(unittest.TestCase):
    def test_every_target_is_built_and_seeded(self):
        names = fuzz.targets(REPO)
        self.assertIn("fuzz_services_json", names)
        cmake = (REPO / "fuzz" / "CMakeLists.txt").read_text()
        for name in names:
            self.assertIn(name, cmake, name)
            seeds = REPO / "fuzz" / "corpus" / name
            self.assertTrue(any(seeds.iterdir()), f"{name}: no seeds")
        for d in set(fuzz.DICTS.values()) | {"json.dict"}:
            self.assertTrue((REPO / "fuzz" / "dict" / d).is_file(), d)

    def test_select(self):
        all_targets = ["fuzz_a_json", "fuzz_b_json", "fuzz_dhcp"]
        self.assertEqual(fuzz.select(all_targets, None), all_targets)
        self.assertEqual(fuzz.select(all_targets, ["dhcp"]), ["fuzz_dhcp"])
        self.assertEqual(fuzz.select(all_targets, ["json"]), all_targets[:2])
        with self.assertRaises(ValueError):
            fuzz.select(all_targets, ["nope"])


class CommandsTest(unittest.TestCase):
    def test_configure_uses_clang_the_sdk_sysroot_and_vcpkg(self):
        cmd = " ".join(fuzz.configure_command(ROOT, CTX))
        self.assertIn("-S /ws/fuzz -B /ws/build/fuzz/build", cmd)
        self.assertIn("-DCMAKE_CXX_COMPILER=clang++-18", cmd)
        self.assertIn("sysroots/core2-64-poky-linux", cmd)
        self.assertIn(
            "-DAEMBER_VCPKG_INSTALLED=/ws/build/configure/x64-poky-linux/Debug/"
            "vcpkg_installed/x64-poky-linux",
            cmd,
        )

    def test_run_grows_the_build_corpus_from_the_seeds(self):
        cmd = fuzz.run_command(ROOT, "fuzz_dhcp", 30)
        self.assertEqual(cmd[0], "/ws/build/fuzz/build/fuzz_dhcp")
        self.assertIn("-max_total_time=30", cmd)
        self.assertIn("-dict=/ws/fuzz/dict/dhcp.dict", cmd)
        self.assertIn("-artifact_prefix=/ws/build/fuzz/crashes/fuzz_dhcp/", cmd)
        # The first corpus directory is the one new inputs go to.
        self.assertEqual(
            cmd[-2:],
            ["/ws/build/fuzz/corpus/fuzz_dhcp", "/ws/fuzz/corpus/fuzz_dhcp"],
        )
        json_cmd = fuzz.run_command(ROOT, "fuzz_ipc", 30)
        self.assertIn("-dict=/ws/fuzz/dict/json.dict", json_cmd)


class ResultTest(unittest.TestCase):
    OUTPUT = (
        "#2048 pulse  cov: 120 ft: 300 corp: 40/1Kb\n"
        "#9000 DONE   cov: 133 ft: 350 corp: 44/1Kb\n"
        "stat::number_of_executed_units: 9000\n"
    )

    def test_parse(self):
        r = fuzz.parse_result("fuzz_dhcp", self.OUTPUT, True, [])
        self.assertTrue(r.ok)
        self.assertEqual((r.runs, r.coverage), (9000, 133))

    def test_a_saved_input_fails_even_with_exit_0(self):
        r = fuzz.parse_result("t", self.OUTPUT, True, [Path("crash-1")])
        self.assertFalse(r.ok)
        self.assertFalse(fuzz.parse_result("t", "", False, []).ok)

    def test_summary(self):
        text = fuzz.summary(
            [
                fuzz.Result("fuzz_a", True, 12345, 10, []),
                fuzz.Result("fuzz_b", False, 5, 2, [Path("crash-1")]),
            ],
            60,
        )
        self.assertIn("| fuzz_a | ok | 12,345 | 10 |", text)
        self.assertIn("| fuzz_b | **FAILED** (1 input(s)) | 5 | 2 |", text)


if __name__ == "__main__":
    unittest.main()
