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
    # Instrumented test build (`aember test --coverage/--sanitize`):
    # "coverage", "asan" (+ UBSan) or "tsan"; None for a plain build.
    instrument: str | None = None

    root: Path = Path.cwd()

    # free-form shared state
    data: Dict[str, Any] = None

    @property
    def variant(self) -> str:
        """Directory name under build/{configure,install}/<arch>/: the build
        type, plus the instrumentation (Debug-asan) so variants never share
        a build tree."""
        if self.instrument:
            return f"{self.build_type}-{self.instrument}"
        return self.build_type

    def __post_init__(self):
        if self.data is None:
            self.data = {}
