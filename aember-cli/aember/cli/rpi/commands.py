# aember/cli/rpi/commands.py

from pathlib import Path

import typer  # type: ignore

from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.rpi import flash as flash_step
from aember.steps.rpi.image import RpiImageStep, image_path
from aember.steps.rpi.launch import RpiLaunchStep

ARCH = "aarch64-poky-linux"


def image(
    build_type: BuildType = typer.Option(
        BuildType.release, help="Which packed build to put on the card"
    ),
):
    """Raspberry Pi 4 SD card image from the packed aarch64 system."""
    ctx = Context(architecture=ARCH, build_type=build_type.value)
    Pipeline([RpiImageStep()]).run(ctx)


def flash(
    device: str = typer.Option(..., "--device", "-d", help="The card, e.g. /dev/sdX"),
    image_file: Path = typer.Option(None, "--image", help="Default: the built image"),
    yes: bool = typer.Option(False, "--yes", help="Don't ask before writing"),
    force: bool = typer.Option(
        False, "--force", help="Allow non-removable or large devices"
    ),
):
    """Write the SD image to a card (on WSL: `usbipd attach --wsl` first)."""
    root = Context().root
    try:
        flash_step.flash(
            image_file or image_path(root),
            device,
            yes=yes,
            force=force,
            confirm=lambda text: typer.confirm(text, default=False),
        )
    except (RuntimeError, ValueError, FileNotFoundError) as e:
        typer.echo(f"aember rpi flash: {e}", err=True)
        raise typer.Exit(1) from None


def launch(
    timeout: int = typer.Option(None, "--timeout", help="Stop after N seconds"),
):
    """Smoke-test the SD image in QEMU raspi3b (no Pi 4 machine in QEMU 8.2)."""
    ctx = Context(architecture=ARCH)
    ctx.data["timeout"] = timeout
    Pipeline([RpiLaunchStep()]).run(ctx)
