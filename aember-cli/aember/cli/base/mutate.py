# aember/cli/base/mutate.py

import json

import typer  # type: ignore

from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.testing.mutate import MutateStep, find_mutants, test_units


def mutate(
    unit: str = typer.Option(
        None,
        "--unit",
        "-k",
        help="Library or service to mutate: its directory name (process-manager, "
        "containerd, netd, ...) or one of its test suites (dhcp-client); every "
        "suite of its tests/ runs against each mutant",
    ),
    max_mutants: int = typer.Option(
        50, "--max", "-n", help="At most this many mutants (a random sample); 0: all"
    ),
    seed: int = typer.Option(1, "--seed", help="Sample seed (same seed, same set)"),
    timeout: int = typer.Option(120, "--timeout", help="Seconds per suite run"),
    file: list[str] = typer.Option(
        None, "--file", "-f", help="Only sources whose path contains this"
    ),
    root: bool = typer.Option(
        False,
        "--root",
        help="Run the suites through `sudo -n` so the root-only tests count too",
    ),
    list_units: bool = typer.Option(
        False, "--list", help="List the units, their suites and mutant counts"
    ),
    as_json: bool = typer.Option(False, "--json", help="With --list: as JSON"),
):
    """Mutation testing: change one operator at a time in a unit's sources,
    rebuild its test suites and run them. Survivors are lines no test checks
    (report: build/mutation/<unit>.json and .md). Needs the x64 Debug build
    with tests (`aember test --build`)."""
    ctx = Context(architecture="x64-poky-linux", build_type="Debug")
    if list_units:
        rows = [
            {
                "unit": u.name,
                "suites": list(u.suites),
                "mutants": sum(
                    len(find_mutants(str(s), s.read_text())) for s in u.sources
                ),
            }
            for u in test_units(ctx.root)
        ]
        if as_json:
            typer.echo(json.dumps(rows))
        else:
            for r in rows:
                typer.echo(f"{r['unit']:20} {r['mutants']:4}  {' '.join(r['suites'])}")
        return
    if not unit:
        raise typer.BadParameter("give a unit with -k (or --list)")
    Pipeline([MutateStep(unit, max_mutants, seed, timeout, file or None, root)]).run(
        ctx
    )
