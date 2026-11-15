# aember/cli/changelog.py

import typer  # type: ignore
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.changelog.changelog import ChangelogStep, VersionBump


def changelog(
    version_bump: VersionBump = typer.Option(
        VersionBump.MINOR,
        help="Version bump type",
        case_sensitive=False,
    )
):
    ctx = Context(bump=version_bump.value)

    Pipeline(
        [
            ChangelogStep(),
        ]
    ).run(ctx)
