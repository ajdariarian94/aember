import subprocess
from pathlib import Path
from typing import Sequence


class CommandRunner:
    @staticmethod
    def run(
        cmd: Sequence[str] | str,
        cwd: Path | None = None,
        shell: bool = False,
        env: dict[str, str] | None = None,
        input: bytes | str | None = None,
    ) -> subprocess.CompletedProcess:
        return subprocess.run(
            cmd if shell else list(cmd),
            cwd=cwd,
            check=True,
            shell=shell,
            executable="/bin/bash" if shell else None,
            env=env,
            input=input,
        )
