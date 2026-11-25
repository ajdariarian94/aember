"""`aember mutate`: which mutants are generated, applying them, the score,
crash recovery, units (a tests/ directory with all its suites) and the
reports."""

import tempfile
import unittest
from pathlib import Path

from aember.steps.testing import mutate
from aember.steps.testing.mutate import Mutant, Result

SOURCE = """\
// a == b in a comment stays
#include <vector>
/* block comment: a && b
   still comment: true */
bool F(int a, int b) {
  if (a == b && Ready()) return true;
  log("a != b || c");  // string and comment: untouched
  return !Done(a + 1) || b <= 3;
}
template <typename T> T G();
"""


class FindMutantsTest(unittest.TestCase):
    def setUp(self):
        self.mutants = mutate.find_mutants("x.cpp", SOURCE)

    def test_only_code_lines_and_outside_literals(self):
        lines = {m.line for m in self.mutants}
        self.assertEqual(lines, {6, 8})

    def test_each_operator_gets_its_opposite(self):
        names = sorted(m.name for m in self.mutants)
        self.assertEqual(
            names,
            sorted(
                [
                    "eq->ne",
                    "and->or",
                    "true->false",
                    "drop-not",
                    "plus1->minus1",
                    "or->and",
                    "le->gt",
                ]
            ),
        )

    def test_apply_changes_exactly_one_spot(self):
        m = next(m for m in self.mutants if m.name == "eq->ne")
        out = m.apply(SOURCE).splitlines()[5]
        self.assertEqual(out, "  if (a != b && Ready()) return true;")
        drop = next(m for m in self.mutants if m.name == "drop-not")
        self.assertIn("return Done(a + 1)", drop.apply(SOURCE))

    def test_apply_refuses_a_stale_mutant(self):
        m = Mutant("x.cpp", 6, 0, "==", "!=", "eq->ne")
        with self.assertRaises(AssertionError):
            m.apply(SOURCE)

    def test_template_and_stream_lines_get_no_comparison_mutants(self):
        src = "std::vector<int> v; out << x >> y; a->b < c;\n"
        names = {m.name for m in mutate.find_mutants("y.cpp", src)}
        self.assertFalse(names & {"lt->ge", "gt->le", "le->gt", "ge->lt"})


class ScoreTest(unittest.TestCase):
    def test_killed_timeouts_and_build_errors_count_as_killed(self):
        m = Mutant("x.cpp", 1, 0, "==", "!=", "eq->ne")
        results = [
            Result(m, "killed"),
            Result(m, "timeout"),
            Result(m, "build-error"),
            Result(m, "survived"),
        ]
        self.assertEqual(mutate.score(results), 75.0)
        self.assertIsNone(mutate.score([]))


class FilesTest(unittest.TestCase):
    def test_backup_left_by_a_killed_run_is_restored(self):
        with tempfile.TemporaryDirectory() as t:
            src = Path(t) / "a.cpp"
            src.write_text("mutated")
            (Path(t) / ("a.cpp" + mutate.BACKUP_SUFFIX)).write_text("original")
            mutate.restore_backups([src])
            self.assertEqual(src.read_text(), "original")
            self.assertFalse((Path(t) / "a.cpp.mutate-backup").exists())

    # Regression: restoring kept the backup's old mtime, so make kept the last
    # mutant's object in the normal build.
    def test_regression_restored_file_is_newer_than_the_mutant(self):
        import os
        import time

        with tempfile.TemporaryDirectory() as t:
            src = Path(t) / "a.cpp"
            src.write_text("original")
            backup = Path(t) / ("a.cpp" + mutate.BACKUP_SUFFIX)
            backup.write_text("original")
            old = time.time() - 100
            os.utime(backup, (old, old))
            src.write_text("mutant")  # compiled now
            mutant_mtime = src.stat().st_mtime
            mutate.restore(backup, src)
            self.assertEqual(src.read_text(), "original")
            self.assertGreaterEqual(src.stat().st_mtime, mutant_mtime)

    def test_units_group_every_suite_of_a_tests_directory(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            ipc = root / "libraries" / "utils" / "ipc"
            netd = root / "services" / "netd"
            for d in (ipc, netd):
                (d / "tests").mkdir(parents=True)
                (d / "src").mkdir()
            (ipc / "src" / "hub.cpp").write_text("")
            (ipc / "tests" / "CMakeLists.txt").write_text(
                "add_executable(ipc-unittest src/hub.cpp)"
            )
            (netd / "src" / "dhcp.cpp").write_text("")
            (netd / "src" / "netd.cpp").write_text("")
            (netd / "tests" / "CMakeLists.txt").write_text(
                "aember_test(netd-daemon-unittest)\n"
                "aember_test(dhcp-client-unittest)\n"
                "target_link_libraries(dhcp-client-unittest x)\n"
            )
            # Not a unit: no sources, or outside libraries/ and services/.
            (root / "libraries" / "empty" / "tests").mkdir(parents=True)
            (root / "libraries" / "empty" / "tests" / "CMakeLists.txt").write_text(
                "add_executable(empty-unittest)"
            )
            (root / "build" / "x" / "tests").mkdir(parents=True)

            units = mutate.test_units(root)
            self.assertEqual([u.name for u in units], ["ipc", "netd"])
            self.assertEqual(units[1].suites, ("dhcp-client", "netd-daemon"))
            self.assertEqual(
                units[1].sources, (netd / "src" / "dhcp.cpp", netd / "src" / "netd.cpp")
            )
            # By unit name or by any of its suites.
            self.assertEqual(mutate.find_unit(root, "netd"), units[1])
            self.assertEqual(mutate.find_unit(root, "dhcp-client"), units[1])
            self.assertEqual(
                mutate.find_unit(root, "ipc").sources, (ipc / "src" / "hub.cpp",)
            )
            self.assertIsNone(mutate.find_unit(root, "nope"))

    def test_markdown_report_lists_survivors_with_their_line(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            (root / "a.cpp").write_text("int x;\nreturn a == b || c;\n")
            unit = mutate.Unit("a", ("a",), (root / "a.cpp",))
            m = Mutant("a.cpp", 2, 9, "==", "!=", "eq->ne")
            text = mutate.report_markdown(
                unit, [Result(m, "survived"), Result(m, "killed")], root
            )
        self.assertIn("### a: 50.0% (2 mutants, 1 survived)", text)
        self.assertIn("| `a.cpp:2` | eq->ne | `return a == b \\|\\| c;` |", text)

    def test_suites_run_until_the_first_failure(self):
        step = mutate.MutateStep("x")
        ran = []

        def fake(root, binary):
            ran.append(binary)
            return {"a": "survived", "b": "killed"}.get(binary, "survived")

        step._run_suite = fake
        self.assertEqual(step._run_suites(Path("/"), ["a", "b", "c"]), "killed")
        self.assertEqual(ran, ["a", "b"])
        ran.clear()
        self.assertEqual(step._run_suites(Path("/"), ["a", "c"]), "survived")
        self.assertEqual(ran, ["a", "c"])

    def test_root_runs_get_their_own_pid_namespace(self):
        cmd = ["sudo", "-n", "env", "A=1", "B=x=/y", "setarch", "x86_64", "/ld.so"]
        self.assertEqual(
            mutate.isolated(cmd, as_root=True, timeout=30),
            ["sudo", "-n", "env", "A=1", "B=x=/y"]
            + ["timeout", "-s", "KILL", "30"]
            + mutate.UNSHARE
            + ["sh", "-c", mutate.SANDBOX, "sandbox"]
            + ["setarch", "x86_64", "/ld.so"],
        )
        self.assertEqual(
            mutate.isolated(["/ld.so", "t"], as_root=False), ["/ld.so", "t"]
        )

    def test_regression_a_suite_killing_its_process_group_spares_the_run(self):
        # Regression: a mutant swapped fork()'s `pid == 0`, the suite then
        # killed pid 0 (its process group) and took `aember mutate` with it,
        # leaving the mutant in the source.
        import sys

        step = mutate.MutateStep("x", timeout=10)

        class Runner:
            def __init__(self, **_):
                pass

            def _command(self, root, binary):
                return [sys.executable, "-c", binary]

            def _env(self, root):
                return None

        original = mutate.RunTests
        mutate.RunTests = Runner
        try:
            kill_own_group = "import os, signal; os.kill(0, signal.SIGKILL)"
            self.assertEqual(step._run_suite(Path("/"), kill_own_group), "killed")
            self.assertEqual(step._run_suite(Path("/"), "pass"), "survived")
            step.timeout = 0.5
            hang = "import subprocess; subprocess.run(['sleep', '30'])"
            self.assertEqual(step._run_suite(Path("/"), hang), "timeout")
        finally:
            mutate.RunTests = original

    def test_build_dir(self):
        self.assertEqual(
            mutate.sub_build_dir(Path("/r"), "services/netd/src/x.cpp"),
            Path(
                "/r/build/configure/x64-poky-linux/Debug/services-prefix/src/"
                "services-build"
            ),
        )
        self.assertTrue(
            str(mutate.sub_build_dir(Path("/r"), "libraries/a/src/x.cpp")).endswith(
                "aember-libs-prefix/src/aember-libs-build"
            )
        )


def _ci_script(name):
    """tools/ci/<name>.py as a module (the workflow runs them bare)."""
    import importlib.util

    path = Path(__file__).resolve().parents[2] / "tools" / "ci" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WorkflowScriptsTest(unittest.TestCase):
    def test_matrix_has_every_unit_with_its_first_suite(self):
        units = _ci_script("mutation-units")
        root = Path(__file__).resolve().parents[2]
        matrix = units.matrix(root, [])
        self.assertEqual(
            [m["unit"] for m in matrix], [u.name for u in mutate.test_units(root)]
        )
        netd = next(m for m in matrix if m["unit"] == "netd")
        self.assertEqual(netd["suite"], "dhcp-client")
        self.assertEqual([m["unit"] for m in units.matrix(root, ["ipc"])], ["ipc"])
        with self.assertRaises(SystemExit):
            units.matrix(root, ["no-such-unit"])

    def test_summary_table(self):
        import json

        summary = _ci_script("mutation-summary")
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            for unit, mutants, survived in (("b", 4, 1), ("a", 6, 0)):
                report = {
                    "unit": unit,
                    "score": round(100 * (mutants - survived) / mutants, 1),
                    "mutants": mutants,
                    "survived": [{}] * survived,
                }
                (d / f"{unit}.json").write_text(json.dumps(report))
            (d / "b.md").write_text("### b: survivors here\n")
            text = summary.summary(d)
        self.assertIn("| a | 100.0% | 6 | 0 |\n| b | 75.0% | 4 | 1 |", text)
        self.assertIn("| **all** | **90.0%** | 10 | 1 |", text)
        self.assertIn("### b: survivors here", text)


if __name__ == "__main__":
    unittest.main()
