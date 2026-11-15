# aember/cli/documentation.py

from aember.core.pipeline import Pipeline
from aember.steps.documentation.generate import DocsGenerateStep
from aember.steps.documentation.serve import DocsServeStep


def generate_docs():
    Pipeline(
        [
            DocsGenerateStep(),
        ]
    ).run()


def view_docs():
    Pipeline(
        [
            DocsServeStep(),
        ]
    ).run()
