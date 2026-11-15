from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict


@dataclass
class Context:
    architecture: str | None = None
    build_type: str | None = None
    build_tests: bool = False
    bump: str | None = None
    force: bool = False
    rootfs_size: int = 512

    root: Path = Path.cwd()

    # free-form shared state
    data: Dict[str, Any] = None

    def __post_init__(self):
        if self.data is None:
            self.data = {}
