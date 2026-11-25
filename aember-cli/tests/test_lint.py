"""`aember lint`: findings fail the command (they used to be only logged),
--check changes nothing."""

import unittest
from pathlib import Path
from unittest import mock

from aember.core.context import Context
from aember.steps.linting import lint


class LintStepTest(unittest.TestCase):
    def run_step(self, fix: bool, ok: bool) -> list[str]:
        commands: list[str] = []

        def fake_run(cmd: str) -> bool:
            commands.append(cmd)
            return ok

        with mock.patch.object(lint, "_run", fake_run), mock.patch.object(
            lint, "_find_cpp_files", return_value=[]
        ), mock.patch.object(
            lint, "_cmake_files", return_value=[Path("CMakeLists.txt")]
        ), mock.patch.object(
            lint, "_format_cmake_files"
        ) as fmt:
            step = lint.LintStep(fix=fix)
            if ok:
                step.run(Context(root=Path(".")))
            else:
                with self.assertRaises(RuntimeError):
                    step.run(Context(root=Path(".")))
        self.formatted_cmake = fmt.called
        return commands

    # Regression: lint findings were only logged; the command passed.
    def test_regression_findings_fail_the_command(self):
        self.run_step(fix=False, ok=False)

    def test_clean_tree_passes(self):
        self.run_step(fix=False, ok=True)

    def test_check_mode_changes_nothing(self):
        commands = self.run_step(fix=False, ok=True)
        self.assertTrue(any(c.startswith("black") and "--check" in c for c in commands))
        self.assertFalse(any("--fix" in c for c in commands))
        self.assertTrue(any(c.startswith("cmake-format --check") for c in commands))
        self.assertFalse(self.formatted_cmake)

    def test_fix_mode_fixes(self):
        commands = self.run_step(fix=True, ok=True)
        self.assertTrue(any(c.startswith("ruff") and "--fix" in c for c in commands))
        self.assertTrue(self.formatted_cmake)


if __name__ == "__main__":
    unittest.main()
