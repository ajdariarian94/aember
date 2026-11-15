from abc import ABC, abstractmethod

from aember.core.context import Context


class Step(ABC):
    @abstractmethod
    def run(self, ctx: Context) -> None:
        pass

    def then(self, next_step: "Step"):
        from aember.core.pipeline import Pipeline

        return Pipeline([self, next_step])
