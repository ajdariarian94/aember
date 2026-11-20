# ruff: noqa: E501  (the test data is compiler output, one finding per line)
"""`aember analyze`: parsing findings, the baseline, the tool commands."""

import tempfile
import unittest
from collections import Counter
from pathlib import Path

from aember.steps.analysis import analyze

ROOT = Path("/ws")

OUTPUT = f"""\
{ROOT}/libraries/utils/ipc/src/hub.cpp:10:5: warning: use nullptr [modernize-use-nullptr]
{ROOT}/libraries/utils/ipc/src/hub.cpp:20:5: warning: use nullptr [modernize-use-nullptr]
{ROOT}/libraries/utils/ipc/src/hub.cpp:30:1: note: expanded from here [modernize-use-nullptr]
{ROOT}/libraries/utils/ipc/tests/src/hub.cpp:3:1: warning: ignored [bugprone-x]
{ROOT}/build/configure/x/vcpkg_installed/include/fmt.h:1:1: warning: ignored [bugprone-y]
/usr/include/stdio.h:1:1: warning: ignored [bugprone-z]
{ROOT}/services/netd/src/dhcp-client.cpp:7:3: style: Variable 'x' can be const [constVariable]
44903 warnings generated.
"""


class ParseTest(unittest.TestCase):
    def test_only_own_non_test_code_counts(self):
        found = analyze.parse(OUTPUT, ROOT)
        self.assertEqual(
            found,
            Counter(
                {
                    (
                        "libraries/utils/ipc/src/hub.cpp",
                        "modernize-use-nullptr",
                        "use nullptr",
                    ): 2,
                    (
                        "services/netd/src/dhcp-client.cpp",
                        "constVariable",
                        "Variable 'x' can be const",
                    ): 1,
                }
            ),
        )

    def test_own_source(self):
        self.assertEqual(
            analyze.own_source(f"{ROOT}/executables/aember/main.cpp", ROOT),
            "executables/aember/main.cpp",
        )
        self.assertIsNone(analyze.own_source(f"{ROOT}/examples/x/main.cpp", ROOT))
        self.assertIsNone(analyze.own_source("/elsewhere/libraries/a.cpp", ROOT))


class BaselineTest(unittest.TestCase):
    def test_round_trip_and_compare(self):
        found = analyze.parse(OUTPUT, ROOT)
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "clang-tidy-baseline.txt"
            analyze.write_baseline(path, found, "clang-tidy")
            baseline = analyze.load_baseline(path)
        self.assertEqual(baseline, found)

        # One more of a known finding and a new one fail; a fixed one is
        # reported so it can leave the baseline.
        now = Counter(found)
        key = next(k for k in found if k[1] == "modernize-use-nullptr")
        now[key] += 1
        now[("libraries/a.cpp", "bugprone-new", "msg")] = 1
        del now[next(k for k in found if k[1] == "constVariable")]
        new, fixed = analyze.compare(now, baseline)
        self.assertEqual(sum(new.values()), 2)
        self.assertEqual([k[1] for k in fixed], ["constVariable"])

    def test_missing_baseline_is_empty(self):
        self.assertEqual(analyze.load_baseline(Path("/nonexistent")), Counter())


class CommandTest(unittest.TestCase):
    def test_clang_tidy_gets_the_sdk_libstdcxx(self):
        cmd = analyze.clang_tidy_command(ROOT, ROOT / "db", ["/f.cpp"])
        self.assertEqual(cmd[0], "run-clang-tidy")
        self.assertIn("-extra-arg=--target=x86_64-linux-gnu", cmd)
        self.assertTrue(any("include/c++" in c for c in cmd))
        # <expected> needs the concepts level libstdc++ 15 checks for.
        self.assertIn("-extra-arg=-D__cpp_concepts=202002L", cmd)
        self.assertEqual(cmd[-1], "/f.cpp")

    def test_cppcheck_ignores_the_build_tree(self):
        cmd = analyze.cppcheck_command(ROOT, ROOT / "db")
        self.assertIn(f"--project={ROOT}/db/compile_commands.json", cmd)
        self.assertIn(f"--suppress=*:{ROOT}/build/*", cmd)
        self.assertTrue(any(c.startswith("--template=") and "[{id}]" in c for c in cmd))


if __name__ == "__main__":
    unittest.main()
