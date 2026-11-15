# aember/cli/base/resources.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.resources.fetch_resources import FetchResources


def fetch_resources(
    architecture: Architecture = typer.Option(
        Architecture.x64_linux,
        help="Target architecture",
        case_sensitive=False,
    ),
):
    ctx = Context(architecture=architecture.value)
    Pipeline([FetchResources()]).run(ctx)
