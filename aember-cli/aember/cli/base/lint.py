# aember/cli/lint.py

import typer  # type: ignore

from aember.steps.linting.lint import LintStep


def lint(
    check: bool = typer.Option(
        False,
        "--check",
        help="Only report (CI): change nothing, fail on anything to fix",
    ),
):
    """ruff, black, clang-format, cmake-format, cmakelint. Fixes what the
    tools can fix, unless --check; fails if issues remain."""
    LintStep(fix=not check).run()
