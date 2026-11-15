# aember/logging.py
import logging

from rich.logging import RichHandler


def setup_logging(verbose: bool = False) -> None:
    level = logging.DEBUG if verbose else logging.INFO

    logging.basicConfig(
        level=level,
        format="%(name)s: %(message)s",
        handlers=[
            RichHandler(
                show_time=False,
                show_level=True,
                show_path=True,
                rich_tracebacks=True,
            )
        ],
        force=True,
    )
