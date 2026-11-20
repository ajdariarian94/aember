# aember/cli/base/mutate.py

import typer  # type: ignore

from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.testing.mutate import MutateStep


def mutate(
    suite: str = typer.Option(
        ...,
        "--suite",
        "-k",
        help="Test suite whose library to mutate (e.g. process-manager, "
        "dhcp-client; the <suite>-unittest binary)",
    ),
    max_mutants: int = typer.Option(
        50, "--max", "-n", help="At most this many mutants (a random sample)"
    ),
    seed: int = typer.Option(1, "--seed", help="Sample seed (same seed, same set)"),
    timeout: int = typer.Option(120, "--timeout", help="Seconds per suite run"),
    file: list[str] = typer.Option(
        None, "--file", "-f", help="Only sources whose path contains this"
    ),
):
    """Mutation testing: change one operator at a time in the library's
    sources, rebuild its test suite and run it. Survivors are lines no test
    checks (report: build/mutation/<suite>.json). Needs the x64 Debug build
    with tests (`aember qemu build --with-tests`)."""
    Pipeline([MutateStep(suite, max_mutants, seed, timeout, file or None)]).run(
        Context(architecture="x64-poky-linux", build_type="Debug")
    )
