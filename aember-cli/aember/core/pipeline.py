from typing import Iterable, List

from aember.core.context import Context
from aember.core.step import Step


class Pipeline:
    def __init__(self, steps: Iterable[Step]):
        self.steps: List[Step] = list(steps)

    def then(self, step: Step) -> "Pipeline":
        self.steps.append(step)
        return self

    def run(self, ctx: Context = Context()) -> None:
        for step in self.steps:
            step.run(ctx)
