# aember/cli/lint.py

from aember.steps.linting.lint import LintStep


def lint():
    LintStep().run()
