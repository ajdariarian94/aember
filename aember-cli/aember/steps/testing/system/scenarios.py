"""
The system-test scenarios (`aember systemtest`). Each one boots the packed
image in its own QEMU (`-snapshot`: nothing it does stays) and checks one
behaviour of the whole system.

Adding a scenario: write a function taking a `System`, decorate it with
`@scenario("name", "what it shows")`, boot with `system.boot()` and assert
through the `Vm` (vm.py): console patterns, commands in the debug shell,
the status file. Raise `vm.fail("...")` (or let a `Vm` wait time out) for a
failure. `extra_args` adds QEMU arguments and `append` kernel arguments.

Scenarios of other repositories (a container's own end-to-end test, e.g.
aember-fleet's) live there and are loaded with `aember systemtest
--scenarios <file.py>`; they register the same way. Their inputs (built
containers, a server image) come in with `--input name=value`
(`system.input(name)`), and `system.image_with_containers()` gives them a
copy of the packed image with their containers and files in it.
"""

from __future__ import annotations

import contextlib
import json
import logging
import shutil
import subprocess
from collections.abc import Callable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from aember.steps.launching.launch import QemuProfile
from aember.steps.packaging.rootfs import run_guestfish, tar_in_commands
from aember.steps.testing.system.vm import Vm, vm_command
from aember.utils import aember_package, ed25519

log = logging.getLogger("aember.systemtest")

# Where packages wait to be installed (containerd installs them at its
# start) and where they are installed.
PACKAGES_DIR = Path("var/lib/aember/packages")
INSTALL_DIR = Path("var/lib/aember")
CONTAINERS_JSON = "etc/aember/containers.json"


@dataclass
class System:
    """What a scenario gets: the packed system and a way to boot it."""

    profile: QemuProfile
    package_dir: Path
    log_dir: Path
    timeout_scale: float = 1.0
    kvm: bool | None = None
    name: str = "scenario"
    inputs: Mapping[str, str] = field(default_factory=dict)
    _boots: int = field(default=0, repr=False)

    @property
    def rootfs(self) -> Path:
        """The packed root file system image."""
        return self.package_dir / "aember-rootfs.img"

    def input(self, name: str) -> str:
        """The value of `--input name=value`; a scenario that needs it fails
        without it."""
        if name not in self.inputs:
            raise AssertionError(
                f"scenario {self.name} needs --input {name}=... "
                f"(given: {', '.join(self.inputs) or 'none'})"
            )
        return self.inputs[name]

    def config(self, name: str) -> dict:
        """/etc/aember/<name> as packed (services.json, containers.json):
        from the pack's overlay_root, or from the image itself (an image
        from a release has no overlay_root next to it)."""
        overlay = self.package_dir / "overlay_root"
        if overlay.is_dir():
            path = overlay / "etc" / "aember" / name
            return json.loads(path.read_text()) if path.exists() else {}
        text = self.image_file(f"/etc/aember/{name}")
        return json.loads(text) if text else {}

    def image_file(self, path: str, image: Path | None = None) -> str | None:
        """A text file from the root file system image (guestfish, read
        only); None if it isn't there."""
        image = image or self.rootfs

        def guestfish(command: str) -> str:
            script = [f"add-ro {image}", "run", "mount-ro /dev/sda1 /", command]
            return run_guestfish(script, what=f"{path} in {image}", read_only=True)

        # Two runs: a failed `cat` is an error even with guestfish's "-".
        if guestfish(f'is-file "{path}"').strip() != "true":
            return None
        return guestfish(f'cat "{path}"')

    def image_copy_out(self, path: str, dest: Path, image: Path | None = None) -> Path:
        """Copy file @p path out of the image (e.g. a container image to
        derive a test version from) to @p dest, which must be under the
        repository (guestfish's `docker run` sees only that)."""
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.unlink(missing_ok=True)
        run_guestfish(
            [
                f"add-ro {image or self.rootfs}",
                "run",
                "mount-ro /dev/sda1 /",
                f'download "{path}" "{dest}"',
            ],
            what=f"{path} in {image or self.rootfs}",
            read_only=True,
        )
        return dest

    def image_with(self, tree: Path, label: str) -> Path:
        """A copy of the packed image with @p tree's files put in (they
        replace what's there, owned by root): <log_dir>/<label>.img. Boot it
        with `boot(rootfs=...)`; with `snapshot=False` it keeps what a run
        writes, for a second boot of the same disk."""
        image = self.log_dir / f"{label}.img"
        image.unlink(missing_ok=True)
        # Sparse copy: the 512 MB image is mostly empty.
        subprocess.run(
            ["cp", "--sparse=always", str(self.rootfs), str(image)], check=True
        )
        tars = self.log_dir / f"{label}-tars"
        script = [f"add {image}", "run", "mount /dev/sda1 /"]
        script += tar_in_commands([tree], tars)
        script += ["umount /", "exit"]
        try:
            run_guestfish(script, what=str(image))
        finally:
            shutil.rmtree(tars, ignore_errors=True)
        return image

    def image_with_files(self, files: Mapping[str, str | bytes], label: str) -> Path:
        """A copy of the packed image with @p files (path in the image ->
        content) put in (`image_with`)."""
        tree = self.log_dir / f"{label}-tree"
        shutil.rmtree(tree, ignore_errors=True)
        tree.mkdir(parents=True)
        for path, content in files.items():
            dst = tree / path.lstrip("/")
            dst.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, bytes):
                dst.write_bytes(content)
            else:
                dst.write_text(content)
        return self.image_with(tree, label)

    def image_with_containers(
        self,
        bundles: Sequence[Path],
        files: Mapping[str, str | bytes] | None = None,
        label: str | None = None,
    ) -> Path:
        """
        A copy of the packed image with more containers and files: each of
        @p bundles is a built container (aember-build-helper's
        `build/aember-out/<arch>/<name>/`: <name>.aember and container.json,
        its containers.json entry), put where containerd installs packages
        from at its start (var/lib/aember/packages/) and added to the
        image's containers.json (an entry of the same name is replaced),
        like `aember-cli/run.py install` does for a board's overlay. A
        bundle of the layout before packages (overlay/var/lib/aember/<name>/
        with the image and lxc.container.conf) is made into an unsigned
        package. @p files: path in the image -> content (configs, keys).
        Returns the image (`image_with`).
        """
        label = label or self.name
        tree = self.log_dir / f"{label}-tree"
        shutil.rmtree(tree, ignore_errors=True)
        tree.mkdir(parents=True)
        containers = self.config("containers.json") or {}
        entries = list(containers.get("containers", []))
        for bundle in bundles:
            bundle = Path(bundle)
            entry = json.loads((bundle / "container.json").read_text())
            name = entry["name"]
            target = tree / PACKAGES_DIR / f"{name}{aember_package.EXTENSION}"
            target.parent.mkdir(parents=True, exist_ok=True)
            package = bundle / f"{name}{aember_package.EXTENSION}"
            if package.is_file():
                shutil.copy2(package, target)
            else:
                old = bundle / "overlay" / INSTALL_DIR / name
                if not (old / f"{name}.squashfs").is_file():
                    raise AssertionError(f"{bundle}: no {package.name} in it")
                aember_package.write_package(
                    target,
                    aember_package.container_config(name, entry),
                    aember_package.container_lxc_config(
                        name, (old / "lxc.container.conf").read_text()
                    ),
                    old / f"{name}.squashfs",
                )
            new = {"name": name}
            if entry.get("dependencies"):
                new["dependencies"] = entry["dependencies"]
            if entry.get("services"):  # e.g. ["aember-netd"]
                new["services"] = entry["services"]
            entries = [c for c in entries if c.get("name") != name] + [new]
        containers = {"bridges": containers.get("bridges", []), **containers}
        containers["containers"] = entries
        (tree / CONTAINERS_JSON).parent.mkdir(parents=True, exist_ok=True)
        (tree / CONTAINERS_JSON).write_text(json.dumps(containers, indent=2) + "\n")
        for path, content in (files or {}).items():
            dst = tree / path.lstrip("/")
            dst.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, bytes):
                dst.write_bytes(content)
            else:
                dst.write_text(content)
        return self.image_with(tree, label)

    def services(self) -> list[dict]:
        """services.json's entries (name, dependencies)."""
        return self.config("services.json").get("services", [])

    def service_config(self, name: str) -> dict:
        """The config.json in a service's folder (executable, timeouts)."""
        path = f"usr/lib/aember/services/{name}/config.json"
        for tree in ("services_root", "overlay_root"):
            file = self.package_dir / tree / path
            if file.is_file():
                return json.loads(file.read_text())
        text = self.image_file(f"/{path}")
        return json.loads(text) if text else {}

    def containers(self) -> list[dict]:
        """containers.json's entries (name, dependencies)."""
        return self.config("containers.json").get("containers", [])

    def container_config(self, name: str) -> dict:
        """The config.json of an installed container (how it starts)."""
        overlay = self.package_dir / "overlay_root"
        path = f"{INSTALL_DIR}/{name}/config.json"
        if overlay.is_dir():
            file = overlay / path
            return json.loads(file.read_text()) if file.exists() else {}
        text = self.image_file(f"/{path}")
        return json.loads(text) if text else {}

    @contextlib.contextmanager
    def boot(
        self,
        append: str = "",
        extra_args: Sequence[str] = (),
        rootfs: Path | None = None,
        snapshot: bool = True,
    ) -> Iterator[Vm]:
        """Boot the system (@p rootfs: another image, e.g. from
        `image_with_containers`; @p snapshot False: the run's writes stay on
        it); the console goes to <log_dir>/<scenario>.log (-2, -3, ... for
        later boots in the same scenario)."""
        self._boots += 1
        suffix = "" if self._boots == 1 else f"-{self._boots}"
        cmd = vm_command(
            self.profile,
            self.package_dir,
            append,
            extra_args,
            self.kvm,
            rootfs=rootfs,
            snapshot=snapshot,
        )
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


def boot_timeline(status: dict) -> list[tuple[int, str, str]]:
    """(since_ms, name, state) of every entry in the status file, in the
    order they reached their state: since_ms is when PID 1 saw the last
    change, in ms since the kernel started (CLOCK_BOOTTIME)."""
    entries = status.get("services", [])
    timeline = [
        (int(e["since_ms"]), e["name"], e["state"])
        for e in entries
        if isinstance(e.get("since_ms"), int)
    ]
    return sorted(timeline)


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
        timeline = boot_timeline(status)
        if len(timeline) != len(states):
            raise vm.fail(f"entries without since_ms in the status: {status}")
        for ms, name, state in timeline:
            log.info("[boot] %7.3f s  %s %s", ms / 1000, name, state)
        last_ms, last, _ = timeline[-1]
        log.info(
            "[boot] all %d running %.3f s after the kernel started (last: %s)",
            len(timeline),
            last_ms / 1000,
            last,
        )
        (system.log_dir / "boot-timeline.json").write_text(
            json.dumps(
                [{"since_ms": t, "name": n, "state": st} for t, n, st in timeline],
                indent=2,
            )
            + "\n"
        )


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
    exe = system.service_config(KILLED_SERVICE)["executable"]
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


def _container_app(config: dict) -> str:
    """The text of the container's own PID 1 in `ps` (aember-init runs its
    command; see the container's config.json)."""
    return "aember-init " + " ".join(config.get("command", []))


@scenario(
    "container-restart",
    "A container's application is killed; containerd starts the container again",
)
def container_restart(system: System) -> None:
    configs = [system.container_config(c["name"]) for c in system.containers()]
    candidates = [
        c for c in configs if c.get("restart_policy") == "always" and c.get("command")
    ]
    if not candidates:
        raise AssertionError("no container with restart_policy always and a command")
    config = candidates[0]
    name, app = config["name"], _container_app(config)
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


# The key the package-install scenario signs with (a test key: its seed is
# here for anyone to read).
TEST_KEY_SEED = bytes(range(1, 33))


@scenario(
    "package-install",
    "containerd installs a signed .aember package at its start (the old "
    "version kept for a rollback) and refuses an unsigned one",
)
def package_install(system: System) -> None:
    containers = system.containers()
    if not containers:
        raise AssertionError("containers.json lists no container")
    name = containers[0]["name"]
    work = system.log_dir / "package-install-work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    installed = f"/{INSTALL_DIR}/{name}"
    image = system.image_copy_out(
        f"{installed}/image.squashfs", work / "image.squashfs"
    )
    config = system.container_config(name)
    lxc = system.image_file(f"{installed}/lxc.container.conf") or ""

    signed = work / f"{name}.aember"
    footer = aember_package.write_package(signed, config, lxc, image, TEST_KEY_SEED)
    rogue_config = dict(config, name="rogue")
    rogue = work / "rogue.aember"
    aember_package.write_package(rogue, rogue_config, lxc, image)  # unsigned

    board = dict(system.config("containers.json"), allow_unsigned=False)
    public = aember_package.public_key_text(ed25519.public_key(TEST_KEY_SEED))
    disk = system.image_with_files(
        {
            f"/{PACKAGES_DIR}/{signed.name}": signed.read_bytes(),
            f"/{PACKAGES_DIR}/{rogue.name}": rogue.read_bytes(),
            "/etc/aember/keys/systemtest.pub": public,
            f"/{CONTAINERS_JSON}": json.dumps(board, indent=2) + "\n",
        },
        "package-install",
    )
    with system.boot(rootfs=disk) as vm:
        vm.wait_ready(UP_TIMEOUT)
        vm.open_shell()
        wait_all_running(system, vm)
        now = json.loads(vm.sh(f"cat {installed}/package.json"))
        if now.get("signed_by") != "systemtest":
            raise vm.fail(f"{name} wasn't installed from the signed package: {now}")
        if now.get("digest") != footer.digest.hex():
            raise vm.fail(f"{name}: installed digest {now.get('digest')}")
        prev = json.loads(vm.sh(f"cat {installed}.prev/package.json"))
        if prev.get("signed_by") is not None:
            raise vm.fail(f"{name}.prev isn't the version from the image: {prev}")
        listing = vm.sh(f"ls /{PACKAGES_DIR}").split()
        if listing != ["rogue.aember.rejected"]:
            raise vm.fail(f"/{PACKAGES_DIR} holds {listing}")
        code, _ = vm.run(f"test -e /{INSTALL_DIR}/rogue")
        if code == 0:
            raise vm.fail("the unsigned package was installed")
