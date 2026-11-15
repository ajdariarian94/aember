import logging

from aember.core.context import Context
from aember.core.step import Step
from aember.utils.check_resources import check_resources

log = logging.getLogger("aember.check_resources")


class CheckResources(Step):

    def run(self, ctx: Context) -> None:
        log.info("Checking resources")
        check_resources(ctx.root)
