"""
The system-test scenarios (`aember systemtest`). Each one boots the packed
image in its own QEMU (`-snapshot`: nothing it does stays) and checks one
behaviour of the whole system.

Adding a scenario: write a function taking a `System`, decorate it with
`@scenario("name", "what it shows")`, boot with `system.boot()` and assert
through the `Vm` (vm.py): console patterns, commands in the debug shell,
the status file. Raise `vm.fail("...")` (or let a `Vm` wait time out) for a
failure. Scenarios elsewhere (e.g. a fleet end-to-end test that also starts
a fleet server) register the same way; `extra_args` adds QEMU arguments and
`append` kernel arguments.
"""

from __future__ import annotations

import contextlib
import json
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from aember.steps.launching.launch import QemuProfile
from aember.steps.testing.system.vm import Vm, vm_command


@dataclass
class System:
    """What a scenario gets: the packed system and a way to boot it."""

    profile: QemuProfile
    package_dir: Path
    log_dir: Path
    timeout_scale: float = 1.0
    kvm: bool | None = None
    name: str = "scenario"
    _boots: int = field(default=0, repr=False)

    def config(self, name: str) -> dict:
        """/etc/aember/<name> as packed (services.json, containers.json)."""
        path = self.package_dir / "overlay_root" / "etc" / "aember" / name
        return json.loads(path.read_text()) if path.exists() else {}

    def services(self) -> list[dict]:
        return self.config("services.json").get("processes", [])

    def containers(self) -> list[dict]:
        return self.config("containers.json").get("containers", [])

    @contextlib.contextmanager
    def boot(self, append: str = "", extra_args: Sequence[str] = ()) -> Iterator[Vm]:
        """Boot the system; the console goes to <log_dir>/<scenario>.log
        (-2, -3, ... for later boots in the same scenario)."""
        self._boots += 1
        suffix = "" if self._boots == 1 else f"-{self._boots}"
        cmd = vm_command(self.profile, self.package_dir, append, extra_args, self.kvm)
        with Vm(
            cmd, self.log_dir / f"{self.name}{suffix}.log", self.timeout_scale
        ) as vm:
            yield vm


@dataclass(frozen=True)
class Scenario:
    name: str
    description: str
    run: Callable[[System], None]


SCENARIOS: dict[str, Scenario] = {}


def scenario(name: str, description: str):
    def register(fn: Callable[[System], None]) -> Callable[[System], None]:
        if name in SCENARIOS:
            raise ValueError(f"system-test scenario '{name}' exists twice")
        SCENARIOS[name] = Scenario(name, description, fn)
        return fn

    return register


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------

RUNNING = "running"
# How long the whole system may take to come up (TCG on a 4-core runner is
# several times slower than KVM; --timeout-scale stretches all of these).
UP_TIMEOUT = 90


def wait_all_running(system: System, vm: Vm) -> dict[str, str]:
    """Every service in services.json and container in containers.json is
    running (per the status file)."""
    expected = [s["name"] for s in system.services()]
    expected += [c["name"] for c in system.containers()]

    def not_running() -> list[str]:
        states = vm.states()
        return [
            f"{n}={states.get(n, 'missing')}"
            for n in expected
            if states.get(n) != RUNNING
        ]

    vm.wait_until(lambda: not not_running(), UP_TIMEOUT, "all running")
    return vm.states()


@scenario("boot", "Init comes up; every service and container reaches running")
def boot(system: System) -> None:
    with system.boot() as vm:
        vm.wait_ready(UP_TIMEOUT)
        vm.open_shell()
        states = wait_all_running(system, vm)
        if not states:
            raise vm.fail("the status file lists no services")
        status = vm.status()
        if "timestamp" not in status:
            raise vm.fail(f"no heartbeat timestamp in the status: {status}")


@scenario(
    "debug-shell",
    "d opens a root shell on the console; exit closes it, d again reopens it",
)
def debug_shell(system: System) -> None:
    with system.boot() as vm:
        vm.wait_ready(UP_TIMEOUT)
        vm.open_shell()
        if (comm := vm.sh("cat /proc/1/comm")) != "aember":
            raise vm.fail(f"PID 1 is '{comm}', not aember")
        if vm.sh("id -u") != "0":
            raise vm.fail("the debug shell isn't root")
        if "services.json" not in vm.sh("ls /etc/aember").split():
            raise vm.fail("/etc/aember has no services.json")
        code, _ = vm.run("false")
        if code != 1:
            raise vm.fail(f"exit codes don't come through ({code})")
        vm.close_shell()
        vm.open_shell()
        vm.sh("true")


# The service killed by service-restart: restarts on failure, and its
# executable is a single process (containerd forks one per container).
KILLED_SERVICE = "aember-netd"


@scenario("service-restart", f"{KILLED_SERVICE} is killed; PID 1 starts it again")
def service_restart(system: System) -> None:
    service = next((s for s in system.services() if s["name"] == KILLED_SERVICE), None)
    if not service:
        raise AssertionError(f"{KILLED_SERVICE} isn't in services.json")
    exe = service["executable"]
    with system.boot() as vm:
        vm.wait_ready(UP_TIMEOUT)
        vm.open_shell()
        wait_all_running(system, vm)
        old = vm.pids(exe)
        if len(old) != 1:
            raise vm.fail(f"expected one {exe}, found {old}")
        vm.sh(f"kill -9 {old[0]}")
        new = vm.wait_until(
            lambda: [p for p in vm.pids(exe) if p != old[0]],
            60,
            f"a new {KILLED_SERVICE}",
        )
        vm.wait_until(
            lambda: vm.states().get(KILLED_SERVICE) == RUNNING,
            60,
            f"{KILLED_SERVICE} running again (pid {new})",
        )


def _container_app(container: dict) -> str:
    """The text of the container's own PID 1 in `ps` (aember-init runs its
    args; see the container's lxc.container.conf)."""
    args = container.get("container", {}).get("args", [])
    return "aember-init " + " ".join(args)


@scenario(
    "container-restart",
    "A container's application is killed; containerd starts the container again",
)
def container_restart(system: System) -> None:
    candidates = [
        c
        for c in system.containers()
        if c.get("restart_policy") == "always" and c.get("container", {}).get("args")
    ]
    if not candidates:
        raise AssertionError("no container with restart_policy always and args")
    container = candidates[0]
    name, app = container["name"], _container_app(container)
    with system.boot() as vm:
        vm.wait_ready(UP_TIMEOUT)
        vm.open_shell()
        wait_all_running(system, vm)
        old = vm.pids(app)
        if not old:
            raise vm.fail(f"no process '{app}' for {name}")
        vm.sh(f"kill -9 {old[0]}")
        vm.wait_until(
            lambda: [p for p in vm.pids(app) if p not in old],
            90,
            f"{name} started again",
        )
        vm.wait_until(
            lambda: vm.states().get(name) == RUNNING, 60, f"{name} running again"
        )


@scenario("poweroff", "kill -USR2 1: PID 1 stops everything and powers off; QEMU ends")
def poweroff(system: System) -> None:
    _shutdown(system, "-USR2", "poweroff", r"reboot: Power down")


@scenario(
    "reboot", "kill -TERM 1: PID 1 stops everything and reboots (QEMU -no-reboot ends)"
)
def reboot(system: System) -> None:
    _shutdown(system, "-TERM", "reboot", r"reboot: Restarting system")


def _shutdown(system: System, signal: str, action: str, kernel_says: str) -> None:
    with system.boot() as vm:
        vm.wait_ready(UP_TIMEOUT)
        vm.open_shell()
        wait_all_running(system, vm)
        start = vm.console.mark()
        # Close the shell first: while it's open, PID 1 keeps its log off
        # the console. The signal comes a second later from the background.
        vm.send(f"(sleep 1; kill {signal} 1) & exit\n")
        vm.wait_for(
            rf"shutting down: {action}", 30, start, what=f"'shutting down: {action}'"
        )
        vm.wait_for(
            r"Aember Init System stopped", 60, start, what="'init stopped' (clean stop)"
        )
        vm.wait_for(kernel_says, 60, start, what=f"'{kernel_says}'")
        if (code := vm.wait_exit(30)) != 0:
            raise vm.fail(f"QEMU exited {code}")
        if "reboot(2) failed" in vm.console.text[start:]:
            raise vm.fail("reboot(2) failed")
