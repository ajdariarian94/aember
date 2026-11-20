"""`aember mutate`: which mutants are generated, applying them, the score,
crash recovery and finding a suite's sources."""

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

    def test_suite_sources_and_build_dir(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            lib = root / "libraries" / "utils" / "ipc"
            (lib / "tests").mkdir(parents=True)
            (lib / "src").mkdir()
            (lib / "src" / "hub.cpp").write_text("")
            (lib / "tests" / "CMakeLists.txt").write_text(
                "add_executable(ipc-unittest src/hub.cpp)"
            )
            self.assertEqual(
                mutate.suite_sources(root, "ipc"), [lib / "src" / "hub.cpp"]
            )
            self.assertEqual(mutate.suite_sources(root, "nope"), [])
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


if __name__ == "__main__":
    unittest.main()
