import json
import logging
import subprocess
from pathlib import Path

from aember.core.context import Context

log = logging.getLogger("aember.compile_commands")


class CompileCommandsMerger:

    @staticmethod
    def merge(ctx: Context) -> Path:
        """
        Find and merge every compile_commands.json under the architecture's
        build directory (one per ExternalProject sub-build) into a single
        file inside that same build directory so clangd can index everything.
        """
        build_dir = ctx.root / "build" / "configure" / ctx.architecture / ctx.build_type

        sources = CompileCommandsMerger._find(build_dir)
        output = build_dir / "compile_commands.json"

        if not sources:
            log.warning("No compile_commands.json files found under %s", build_dir)
            return output

        merged = CompileCommandsMerger._merge_entries(sources)
        output.write_text(json.dumps(merged, indent=2))

        log.info(
            "Merged %d compile commands from %d file(s) into %s",
            len(merged),
            len(sources),
            output,
        )
        return output

    @staticmethod
    def _find(build_dir: Path) -> list[Path]:
        result = subprocess.run(
            ["find", str(build_dir), "-name", "compile_commands.json"],
            capture_output=True,
            text=True,
            check=True,
        )
        return [Path(p) for p in result.stdout.splitlines() if p]

    @staticmethod
    def _merge_entries(sources: list[Path]) -> list[dict]:
        merged: list[dict] = []
        for src in sources:
            entries = json.loads(src.read_text())
            merged.extend(entries)
            log.debug("Merged %d entries from %s", len(entries), src)
        return merged
