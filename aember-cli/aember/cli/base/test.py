# aember/cli/base/test.py

import typer  # type: ignore

from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.testing.run_cli_tests import RunCliTests
from aember.steps.testing.run_tests import RunTests


def test(
    filter: str = typer.Option(
        None, "--filter", "-k", help="Only suites whose name contains this"
    ),
    repeat: int = typer.Option(1, "--repeat", "-n", help="Run everything N times"),
    timeout: int = typer.Option(300, "--timeout", help="Seconds per suite"),
    cli: bool = typer.Option(
        True, "--cli/--no-cli", help="Also run the aember-cli Python tests"
    ),
):
    """Run the aember-cli tests and the C++ unit tests."""
    steps = []
    # -k filters C++ suites; the CLI tests run with it only for "cli".
    if cli and (filter is None or filter in "aember-cli"):
        steps.append(RunCliTests())
    steps.append(RunTests(filter=filter, repeat=repeat, timeout=timeout))
    Pipeline(steps).run(Context())
