import logging
import re
import subprocess
from datetime import datetime
from enum import Enum
from pathlib import Path

from aember.core.context import Context
from aember.core.step import Step

log = logging.getLogger("aember.changelog")


class VersionBump(Enum):
    MAJOR = "major"
    MINOR = "minor"
    PATCH = "patch"


class ChangelogStep(Step):

    # ------------------------------------------------------------------ #
    # Step API
    # ------------------------------------------------------------------ #

    def run(self, ctx: Context) -> None:
        self.bump = VersionBump(ctx.bump)
        self.base_branch = "main"
        self.cmake_file = "CMakeLists.txt"

        log.info("Running changelog step (bump=%s)", self.bump)

        commits = self._get_commits()
        files = self._get_changed_files()
        submodule_changes = self._get_submodule_changes()

        if not commits and not files and not submodule_changes:
            log.warning(
                "No changes detected against %s (including submodules)",
                self.base_branch,
            )
            return

        cmake_path = ctx.root / self.cmake_file
        old_version = self._extract_version(cmake_path)
        new_version = self._bump_version(old_version)

        self._update_cmake(cmake_path, new_version)

        changelog = self._write_changelog(
            ctx, old_version, new_version, commits, files, submodule_changes
        )

        log.info("Version bumped: %s → %s", old_version, new_version)
        log.info("Changelog written: %s", changelog)
        log.info("Changelog step completed successfully")

    # ------------------------------------------------------------------ #
    # Git helpers
    # ------------------------------------------------------------------ #

    def _run_git(self, cmd: str) -> str:
        return subprocess.check_output(
            cmd,
            shell=True,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()

    def _get_commits(self) -> list[str]:
        log.debug("Collecting superproject commits from %s..HEAD", self.base_branch)
        out = self._run_git(f"git log {self.base_branch}..HEAD --pretty=format:'%h %s'")
        return out.splitlines() if out else []

    def _get_changed_files(self) -> list[str]:
        log.debug(
            "Collecting superproject file changes from %s..HEAD", self.base_branch
        )
        out = self._run_git(f"git diff --name-only {self.base_branch}..HEAD")
        return out.splitlines() if out else []

    def _get_submodules(self) -> list[str]:
        out = self._run_git("git submodule status --recursive")
        if not out:
            return []
        # format: " 1a2b3c path (branch)"
        return [line.split()[1] for line in out.splitlines()]

    def _get_submodule_changes(self) -> dict[str, dict[str, list[str]]]:
        changes: dict[str, dict[str, list[str]]] = {}

        for submodule in self._get_submodules():
            log.debug("Checking submodule: %s", submodule)
            try:
                commits_out = self._run_git(
                    f"git -C {submodule} log {self.base_branch}..HEAD "
                    f"--pretty=format:'%h %s'"
                )
                files_out = self._run_git(
                    f"git -C {submodule} diff --name-only {self.base_branch}..HEAD"
                )
            except subprocess.CalledProcessError:
                log.debug(
                    "Submodule %s does not have base branch %s",
                    submodule,
                    self.base_branch,
                )
                continue

            if commits_out or files_out:
                changes[submodule] = {
                    "commits": commits_out.splitlines() if commits_out else [],
                    "files": files_out.splitlines() if files_out else [],
                }

        return changes

    # ------------------------------------------------------------------ #
    # Version handling
    # ------------------------------------------------------------------ #

    def _extract_version(self, cmake_path: Path) -> str:
        match = re.search(
            r"project\s*\(.*?VERSION\s+(\d+\.\d+\.\d+)",
            cmake_path.read_text(),
            re.IGNORECASE | re.DOTALL,
        )
        if not match:
            raise RuntimeError("VERSION not found in project()")
        return match.group(1)

    def _bump_version(self, version: str) -> str:
        major, minor, patch = map(int, version.split("."))
        match self.bump:
            case VersionBump.MAJOR:
                return f"{major + 1}.0.0"
            case VersionBump.MINOR:
                return f"{major}.{minor + 1}.0"
            case _:
                return f"{major}.{minor}.{patch + 1}"

    def _update_cmake(self, cmake_path: Path, new_version: str) -> None:
        log.debug("Updating version in %s", cmake_path)
        updated, count = re.subn(
            r"(project\s*\(.*?VERSION\s+)(\d+\.\d+\.\d+)",
            rf"\g<1>{new_version}",
            cmake_path.read_text(),
            flags=re.IGNORECASE | re.DOTALL,
            count=1,
        )
        if count != 1:
            raise RuntimeError("Failed to update VERSION in project()")
        cmake_path.write_text(updated)

    # ------------------------------------------------------------------ #
    # Changelog
    # ------------------------------------------------------------------ #

    def _write_changelog(
        self,
        ctx: Context,
        old_version: str,
        new_version: str,
        commits: list[str],
        files: list[str],
        submodules: dict[str, dict[str, list[str]]],
    ) -> Path:
        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
        changelog_dir = ctx.root / "doc" / "changelog"
        changelog_dir.mkdir(parents=True, exist_ok=True)
        path = changelog_dir / f"changes-{ts}.md"

        log.debug("Writing changelog: %s", path)

        lines: list[str] = [
            f"# Changes – {ts}\n",
            f"**Version:** `{old_version}` → `{new_version}`\n",
            f"**Bump type:** `{self.bump.value}`\n",
        ]

        if commits:
            lines += ["\n## Commits (superproject)\n"] + [f"- {c}\n" for c in commits]

        if files:
            lines += ["\n## Files changed (superproject)\n"] + [
                f"- `{f}`\n" for f in files
            ]

        if submodules:
            lines.append("\n## Submodule changes\n")
            for name, data in submodules.items():
                lines.append(f"\n### {name}\n")
                if data["commits"]:
                    lines += ["\nCommits:\n"] + [f"- {c}\n" for c in data["commits"]]
                if data["files"]:
                    lines += ["\nFiles changed:\n"] + [
                        f"- `{f}`\n" for f in data["files"]
                    ]
                lines.append("\n")

        path.write_text("".join(lines))
        return path
