# aember/cli/base/test.py

import typer  # type: ignore

from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.testing.run_tests import RunTests


def test(
    filter: str = typer.Option(
        None, "--filter", "-k", help="Only suites whose name contains this"
    ),
    repeat: int = typer.Option(1, "--repeat", "-n", help="Run everything N times"),
    timeout: int = typer.Option(300, "--timeout", help="Seconds per suite"),
):
    Pipeline([RunTests(filter=filter, repeat=repeat, timeout=timeout)]).run(Context())
