# aember/cli/base/publish.py

import typer  # type: ignore

from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.releasing.publish import PublishRelease


def publish(
    clobber: bool = typer.Option(
        False,
        "--clobber",
        help="Replace assets that already exist in the GitHub release",
    ),
):
    Pipeline([PublishRelease(clobber=clobber)]).run(Context())
