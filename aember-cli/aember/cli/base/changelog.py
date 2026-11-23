# aember/cli/base/changelog.py

import typer  # type: ignore
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.changelog.changelog import ChangelogStep, VersionBump


def changelog(
    version_bump: VersionBump = typer.Option(
        VersionBump.MINOR,
        help="Version bump type",
        case_sensitive=False,
    ),
    since: str = typer.Option(
        "main",
        help="Count the changes from this ref: main on a feature branch, the "
        "last release tag (e.g. v0.12.0) on main.",
    ),
):
    ctx = Context(bump=version_bump.value, data={"since": since})

    Pipeline(
        [
            ChangelogStep(),
        ]
    ).run(ctx)
