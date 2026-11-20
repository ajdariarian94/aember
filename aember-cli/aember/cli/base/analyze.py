# aember/cli/base/analyze.py

import typer  # type: ignore

from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.analysis.analyze import TOOLS, AnalyzeStep
from aember.steps.cmake.build import BuildStep
from aember.steps.cmake.configure import ConfigureStep
from aember.steps.resources.fetch_resources import FetchResources


def analyze(
    tool: list[str] = typer.Option(
        list(TOOLS), "--tool", "-t", help="clang-tidy and/or cppcheck (repeatable)"
    ),
    update_baseline: bool = typer.Option(
        False,
        "--update-baseline",
        help="Accept the current findings (rewrite tools/analysis/*-baseline.txt)",
    ),
    build: bool = typer.Option(
        True, "--build/--no-build", help="Build x64 Debug with tests first"
    ),
):
    """Static analysis (clang-tidy, cppcheck) of libraries/, services/,
    executables/: findings beyond tools/analysis/*-baseline.txt fail."""
    unknown = set(tool) - set(TOOLS)
    if unknown:
        raise typer.BadParameter(f"unknown tool(s): {', '.join(sorted(unknown))}")
    ctx = Context(
        architecture=Architecture.x64_linux.value,
        build_type=BuildType.debug.value,
        build_tests=True,
    )
    steps = [FetchResources(), ConfigureStep(), BuildStep()] if build else []
    steps.append(AnalyzeStep(tuple(tool), update_baseline))
    Pipeline(steps).run(ctx)
