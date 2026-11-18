# aember/cli/launch.py

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.steps.launching.chroot import ChrootLaunchStep
from aember.steps.launching.launch import LaunchStep


def launch(
    architecture: Architecture = typer.Option(
        Architecture.x64_linux,
        help="Target architecture",
        case_sensitive=False,
    ),
    build_type: BuildType = typer.Option(
        BuildType.release,
        help="Build type",
        case_sensitive=False,
    ),
    headless: bool = typer.Option(
        False,
        "--headless",
        help="Run in this terminal without windows (serial console on stdout)",
    ),
    timeout: int = typer.Option(
        None,
        "--timeout",
        help="With --headless: stop QEMU after this many seconds",
    ),
    debug_shell: bool = typer.Option(
        False,
        "--debug-shell",
        help="Open a debug shell on the console at boot (aember.shell=1); "
        "otherwise press 'd' on the console at any time",
    ),
    append: str = typer.Option(
        "",
        "--append",
        help='Extra kernel command line, e.g. "aember.watchdog=0" (QEMU only)',
    ),
    chroot: bool = typer.Option(
        False,
        "--chroot",
        help="Run without QEMU: aember as PID 1 in a chroot of the rootfs image "
        "on the host kernel, containers on the host's X server",
    ),
):
    ctx = Context(architecture=architecture.value, build_type=build_type.value)
    ctx.data.update(
        headless=headless, timeout=timeout, debug_shell=debug_shell, append=append
    )
    (ChrootLaunchStep() if chroot else LaunchStep()).run(ctx)
