# aember/cli/app.py

import typer  # type: ignore
from aember.utils.logging import setup_logging

from .base.appimage import appimage
from .base.build import build
from .base.changelog import changelog
from .base.clean import clean
from .base.configure import configure
from .base.documentation import generate_docs, view_docs
from .base.launch import launch
from .base.lint import lint
from .base.pack import pack
from .base.publish import publish
from .base.release import release
from .base.resources import fetch_resources
from .base.test import test
from .base.analyze import analyze
from .base.fuzz import fuzz
from .base.mutate import mutate
from .base.systemtest import systemtest
from .qemu.build import build_qemu
from .rpi import commands as rpi_commands
from .usb import commands as usb_commands

aember = typer.Typer(help="Aember build system")


@aember.callback()
def main(
    verbose: bool = typer.Option(
        False,
        "--verbose",
        "-v",
        help="Enable verbose logging",
    )
):
    setup_logging(verbose)


# ----------------------------
# Aember sub-app
# ----------------------------
aember_app = typer.Typer(help="Aember related commands")

# ----------------------------
# qemu sub-app
# ----------------------------
qemu_app = typer.Typer(help="qemu related commands")

# ----------------------------
# Base commands
# ----------------------------
aember.command(help="Generate changelog and bump version")(changelog)
aember.command(help="Clean all build artifacts")(clean)
aember.command(help="Lint Python, C++, and CMake code")(lint)
aember.command(help="Generate project documentation")(generate_docs)
aember.command(
    name="fetch-resources",
    help="Download kernel, BusyBox, LXC and SDK from aember-resources",
)(fetch_resources)
aember.command(help="Create a release archive")(release)
aember.command(help="x86-64 AppImage: aember isolated on a Linux desktop")(appimage)
aember.command(help="Upload build/release/ as GitHub release v<VERSION>")(publish)
aember.command(help="Serve documentation locally")(view_docs)
aember.command(
    help="Run the CLI and C++ unit tests; --coverage, --sanitize, --valgrind, --root"
)(test)
aember.command(help="Static analysis: clang-tidy and cppcheck against a baseline")(
    analyze
)
aember.command(help="Mutation testing of one library's test suite")(mutate)
aember.command(help="Fuzz the code that reads input with libFuzzer, ASan and UBSan")(
    fuzz
)
aember.command(
    help="System tests: boot the packed image in QEMU and check the whole system"
)(systemtest)

# ----------------------------
# Aember commands
# ----------------------------
aember_app.command("configure", help="Configure the project using CMake")(configure)
aember_app.command("build", help="Build the project using CMake")(build)
aember_app.command("pack", help="Package initramfs and root filesystem")(pack)
aember_app.command("launch", help="Run the system in QEMU")(launch)

# ----------------------------
# Qemu commands (subcommands)
# ----------------------------
qemu_app.command("build", help="Fetch resources, build and pack the system for QEMU")(
    build_qemu
)
qemu_app.command("launch", help="Boot the packed system in QEMU")(launch)


aember.add_typer(aember_app, name="app")
aember.add_typer(qemu_app, name="qemu")

# ----------------------------
# Raspberry Pi 4
# ----------------------------
rpi_app = typer.Typer(help="Raspberry Pi 4: SD card image, flashing, QEMU test")
rpi_app.command("image")(rpi_commands.image)
rpi_app.command("flash")(rpi_commands.flash)
rpi_app.command("launch")(rpi_commands.launch)
aember.add_typer(rpi_app, name="rpi")

usb_app = typer.Typer(help="x86-64 PCs: bootable USB stick image, flashing, QEMU test")
usb_app.command("image")(usb_commands.image)
usb_app.command("flash")(usb_commands.flash)
usb_app.command("launch")(usb_commands.launch)
aember.add_typer(usb_app, name="usb")
