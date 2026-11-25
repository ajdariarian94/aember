# aember/cli/base/godbolt.py

import logging
import os
from pathlib import Path

import typer  # type: ignore

from aember.config.build_type import BuildType
from aember.steps.godbolt import godbolt as ce

log = logging.getLogger("aember.godbolt")


def godbolt(
    port: int = typer.Option(ce.DEFAULT_PORT, "--port", help="Port on localhost"),
    stop: bool = typer.Option(False, "--stop", help="Stop Compiler Explorer"),
    file: Path = typer.Option(
        None,
        "--file",
        "-f",
        help="Open with this source (e.g. a function copied out of "
        "libraries/...) in the editor",
    ),
    build_type: BuildType = typer.Option(
        BuildType.release,
        help="Whose flags to use (build/configure/<arch>/<type>/compile_commands.json)",
        case_sensitive=False,
    ),
    rebuild_image: bool = typer.Option(
        False, "--rebuild-image", help="Build the Compiler Explorer image again"
    ),
):
    """A local Compiler Explorer (godbolt) with the aember SDK's cross
    compilers for every configured target and aember's own build flags, so
    `#include <aember-libs/...>` works. Runs upstream's Compiler Explorer
    (pinned, tools/godbolt/Dockerfile) in Docker on localhost."""
    root = Path.cwd()
    ce.stop()
    if stop:
        typer.echo("Compiler Explorer stopped")
        return

    compilers = ce.find_compilers(root, build_type.value)
    if not compilers:
        raise typer.BadParameter(
            "no configured target: run `aember app configure` (or a build) first",
            param_hint="--build-type",
        )
    ce.write_config(root / "build" / "godbolt", compilers)
    if rebuild_image or not ce.image_exists():
        ce.build_image(root)

    from aember.config.host_path import HOST_ROOT_PATH

    cmd = ce.docker_run_command(str(HOST_ROOT_PATH), port, os.getuid(), os.getgid())
    ce.subprocess.run(cmd, check=True, capture_output=True)
    ce.wait_ready()

    url = f"http://localhost:{port}/"
    typer.echo(f"Compiler Explorer: {url}")
    typer.echo("Compilers: " + ", ".join(c.id for c in compilers))
    if file:
        typer.echo("With your file:")
        typer.echo(ce.client_state_url(url, file.read_text(), compilers[0].id))
    typer.echo("Stop it with: aember godbolt --stop")
