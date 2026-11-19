# aember/cli/usb/commands.py

from pathlib import Path

import typer  # type: ignore

from aember.config import usb
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.rpi import flash as flash_step
from aember.steps.usb.image import UsbImageStep, image_path
from aember.steps.usb.launch import UsbLaunchStep


def image(
    build_type: BuildType = typer.Option(
        BuildType.release, help="Which packed build to put on the stick"
    ),
):
    """Bootable x86-64 USB stick image (UEFI) from the packed x64 system."""
    ctx = Context(architecture=usb.ARCHITECTURE, build_type=build_type.value)
    Pipeline([UsbImageStep()]).run(ctx)


def flash(
    device: str = typer.Option(..., "--device", "-d", help="The stick, e.g. /dev/sdX"),
    image_file: Path = typer.Option(None, "--image", help="Default: the built image"),
    yes: bool = typer.Option(False, "--yes", help="Don't ask before writing"),
    force: bool = typer.Option(
        False, "--force", help="Allow non-removable or large devices"
    ),
):
    """Write the USB image to a stick (on WSL: `usbipd attach --wsl` first)."""
    root = Context().root
    try:
        flash_step.flash(
            image_file or image_path(root),
            device,
            yes=yes,
            force=force,
            confirm=lambda text: typer.confirm(text, default=False),
            max_bytes=usb.MAX_STICK_BYTES,
            kind="a USB stick",
            build_hint="aember usb image",
            done=(
                "Done: boot the PC from the stick (boot menu, often F12/F10/Esc) "
                "in UEFI mode with Secure Boot off."
            ),
        )
    except (RuntimeError, ValueError, FileNotFoundError) as e:
        typer.echo(f"aember usb flash: {e}", err=True)
        raise typer.Exit(1) from None


def launch(
    headless: bool = typer.Option(
        False, "--headless", help="No window: serial console in this terminal"
    ),
    timeout: int = typer.Option(None, "--timeout", help="Stop after N seconds"),
):
    """Boot the USB image in QEMU like a PC boots the stick (UEFI, USB)."""
    ctx = Context(architecture=usb.ARCHITECTURE)
    ctx.data["headless"] = headless
    ctx.data["timeout"] = timeout
    Pipeline([UsbLaunchStep()]).run(ctx)
