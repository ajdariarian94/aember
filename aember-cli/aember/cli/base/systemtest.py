# aember/cli/base/systemtest.py

from pathlib import Path

import typer  # type: ignore
from aember.config.architecture import Architecture
from aember.config.build_type import BuildType
from aember.core.context import Context
from aember.core.pipeline import Pipeline
from aember.steps.testing.system.scenarios import SCENARIOS
from aember.steps.testing.system.step import (
    SystemTestStep,
    load_scenarios,
    parse_inputs,
)


def systemtest(
    architecture: Architecture = typer.Option(
        Architecture.x64_linux, help="Target architecture", case_sensitive=False
    ),
    build_type: BuildType = typer.Option(
        BuildType.release, help="Build type", case_sensitive=False
    ),
    scenario: list[str] = typer.Option(
        None,
        "--scenario",
        "-k",
        help=f"Only these scenarios (repeatable): {', '.join(SCENARIOS)}",
    ),
    timeout_scale: float = typer.Option(
        1.0,
        "--timeout-scale",
        help="Multiply every wait (e.g. 3 for TCG on a slow CI runner)",
    ),
    kvm: bool = typer.Option(
        None,
        "--kvm/--no-kvm",
        help="QEMU with KVM; default: when /dev/kvm is usable",
    ),
    scenario_files: list[Path] = typer.Option(
        None,
        "--scenarios",
        help="Also load the scenarios in this Python file (repeatable), e.g. "
        "a container repository's end-to-end tests; without -k only those run",
    ),
    inputs: list[str] = typer.Option(
        None,
        "--input",
        help="name=value for the scenarios (repeatable): built containers, "
        "server images, ...",
    ),
    package_dir: Path = typer.Option(
        None,
        "--package-dir",
        help="Boot this kernel/initramfs/rootfs image instead of the packed "
        "build/package/<arch>/<type>/ (e.g. a release's, unpacked under build/)",
    ),
    list_only: bool = typer.Option(False, "--list", help="List the scenarios and exit"),
):
    """Boot the packed image in QEMU once per scenario and check the whole
    system through the serial console and the debug shell (needs `aember app
    build` and `aember app pack`). Consoles: build/system-tests/<arch>/."""
    if list_only:
        load_scenarios(scenario_files or [])
        for s in SCENARIOS.values():
            typer.echo(f"{s.name:20} {s.description}")
        return
    step = SystemTestStep(
        scenario or None,
        timeout_scale,
        kvm,
        scenario_files=scenario_files or [],
        inputs=parse_inputs(inputs or []),
        package_dir=package_dir.resolve() if package_dir else None,
    )
    Pipeline([step]).run(
        Context(architecture=architecture.value, build_type=build_type.value)
    )
