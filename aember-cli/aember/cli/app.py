# aember/cli/app.py

import typer  # type: ignore
from aember.utils.logging import setup_logging

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
from .qemu.build import build_qemu
from .qemu.launch import launch_qemu

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
aember.command(help="Upload build/release/ as GitHub release v<VERSION>")(publish)
aember.command(help="Serve documentation locally")(view_docs)
aember.command(help="Run the C++ unit tests (after `aember qemu build --with-tests`)")(
    test
)

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
qemu_app.command("launch", help="Boot the packed system in QEMU")(launch_qemu)


aember.add_typer(aember_app, name="app")
aember.add_typer(qemu_app, name="qemu")
