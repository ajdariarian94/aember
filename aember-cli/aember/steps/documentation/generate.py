import logging
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step
from aember.utils.runner import CommandRunner

log = logging.getLogger("aember.docs.generate")


class DocsGenerateStep(Step):

    def __init__(self, doxyfile: Path | str = "./doc/Doxyfile"):
        self.doxyfile = Path(doxyfile)

    def run(self, ctx: Context) -> None:
        if not self.doxyfile.exists():
            raise FileNotFoundError(f"Doxyfile not found: {self.doxyfile}")

        log.info("Generating documentation (Doxyfile: %s)", self.doxyfile)
        CommandRunner.run(["doxygen", str(self.doxyfile)])
        log.info("Documentation generated successfully")
