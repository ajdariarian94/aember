"""The system-test harness (`aember systemtest`) without QEMU: the console,
the QEMU command line, and the debug-shell protocol against a fake device
(a Python script that answers like aember's console)."""

import json
import os
import sys
import tempfile
import textwrap
import threading
import unittest
from pathlib import Path
from unittest import mock

from aember.steps.launching.launch import QEMU_PROFILES
from aember.utils import aember_package
from aember.steps.testing.system.scenarios import (
    boot_timeline,
    SCENARIOS,
    System,
    _container_app,
    scenario,
)
from aember.steps.testing.system.step import (
    load_scenarios,
    parse_inputs,
    select,
    summary_markdown,
)
from aember.steps.testing.system.vm import (
    READY,
    Console,
    Vm,
    VmError,
    accel_args,
    vm_command,
)

# Prints the ready line, opens a "shell" on d, runs each line with sh (with
# the console's echo of the line first), "Debug shell exited" on exit. The
# status file path is the first argument.
FAKE_DEVICE = textwrap.dedent(
    r"""
    import subprocess, sys
    status = sys.argv[1]
    # kmsg: a kernel message lands right after the first command's begin
    # marker, before its line end (what the serial console did once).
    kmsg = "kmsg" in sys.argv[2:]
    out = sys.stdout
    out.write("[info] \x1b[32mAember Init System started successfully\x1b[0m\r\n")
    out.flush()
    while True:
        c = sys.stdin.read(1)
        if not c:
            break
        if c != "d":
            continue
        out.write("~ # "); out.flush()
        for line in sys.stdin:
            out.write(line.replace("\n", "\r\n"))
            if line.strip() == "exit":
                out.write("[info] Debug shell exited\r\n"); out.flush()
                break
            line = line.replace("/var/log/aember-status.json", status)
            r = subprocess.run(["sh", "-c", line], capture_output=True, text=True)
            if kmsg and "@@B" in r.stdout:
                junk = "[    0.902669] input: mouse\n"
                r.stdout = r.stdout.replace("@@\n", "@@" + junk + "\n", 1)
                kmsg = False
            out.write(r.stdout.replace("\n", "\r\n") + "~ # "); out.flush()
    """
)


class ConsoleTest(unittest.TestCase):
    def test_escapes_and_line_ends_are_removed(self):
        c = Console()
        c.add("a \x1b[32mgreen\x1b[0m\r\n")
        c.add("cut \x1b[3")  # the sequence goes on in the next chunk
        c.add("1mred\x1b[0m\r\n")
        self.assertEqual(c.text, "a green\ncut red\n")

    def test_wait_for_from_a_mark(self):
        c = Console()
        c.add("ready\n")
        mark = c.mark()
        self.assertIsNone(c.wait_for("ready", 0.05, mark))
        threading.Timer(0.05, c.add, ["ready again\n"]).start()
        self.assertTrue(c.wait_for("ready again", 5, mark))

    def test_closed_console_stops_waiting(self):
        c = Console()
        c.close()
        self.assertIsNone(c.wait_for("never", 30))


class CommandTest(unittest.TestCase):
    def test_headless_snapshot_no_reboot(self):
        cmd = vm_command(
            QEMU_PROFILES["x64-poky-linux"], Path("/p"), "x=1", ["-z"], kvm=False
        )
        self.assertEqual(cmd[0], "qemu-system-x86_64")
        for arg in ("-snapshot", "-no-reboot"):
            self.assertIn(arg, cmd)
        self.assertEqual(cmd[cmd.index("-serial") + 1], "stdio")
        self.assertEqual(cmd[cmd.index("-display") + 1], "none")
        self.assertNotIn("gtk", cmd)
        self.assertIn("file=/p/aember-rootfs.img,if=ide,format=raw", cmd)
        self.assertTrue(cmd[cmd.index("-append") + 1].endswith("x=1"))
        self.assertEqual(cmd[-1], "-z")
        self.assertNotIn("-accel", cmd)

    def test_own_disk_without_snapshot(self):
        cmd = vm_command(
            QEMU_PROFILES["x64-poky-linux"],
            Path("/p"),
            kvm=False,
            rootfs=Path("/l/own.img"),
            snapshot=False,
        )
        self.assertIn("file=/l/own.img,if=ide,format=raw", cmd)
        self.assertNotIn("-snapshot", cmd)
        self.assertIn("-no-reboot", cmd)

    def test_accel(self):
        self.assertEqual(accel_args(True), ["-accel", "kvm"])
        self.assertEqual(accel_args(False), [])
        # Regression: auto chose KVM for aarch64 on an x86 host, and QEMU
        # refused to start ("invalid accelerator kvm").
        if os.uname().machine == "x86_64":
            self.assertEqual(accel_args(None, "qemu-system-aarch64"), [])


class FakeDeviceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.status = self.tmp / "status.json"
        script = self.tmp / "fake.py"
        script.write_text(FAKE_DEVICE)
        self.cmd = [sys.executable, "-u", str(script), str(self.status)]

    def test_shell_commands_exit_codes_and_status(self):
        services = [{"name": "aember-netd", "state": "running", "type": "process"}]
        self.status.write_text(json.dumps({"services": services}))
        with Vm(self.cmd, self.tmp / "console.log") as vm:
            vm.wait_ready(10)
            vm.open_shell()
            self.assertEqual(vm.sh("echo hello; echo world"), "hello\nworld")
            self.assertEqual(vm.run("sh -c 'exit 3'")[0], 3)
            self.assertEqual(vm.run("false")[0], 1)
            with self.assertRaises(VmError):
                vm.sh("false")
            self.assertEqual(vm.states(), {"aember-netd": "running"})
            self.assertEqual(vm.wait_until(lambda: 5, 1, "five"), 5)
            with self.assertRaises(VmError) as e:
                vm.wait_until(lambda: [], 0.1, "never", interval=0.01)
            self.assertIn("never", str(e.exception))
            vm.close_shell()
        self.assertIn(READY, (self.tmp / "console.log").read_text())

    def test_kernel_message_after_the_begin_marker(self):
        # Regression: a kernel message printed between a command's begin
        # marker and its line end made the marker unmatched, so open_shell's
        # `dmesg -n 1` (which keeps them off) failed the boot.
        with Vm(self.cmd + ["kmsg"], self.tmp / "console.log") as vm:
            vm.wait_ready(10)
            vm.open_shell()
            self.assertEqual(vm.sh("echo hello"), "hello")

    def test_a_missing_pattern_fails_with_the_console(self):
        with Vm(self.cmd, self.tmp / "console.log") as vm:
            with self.assertRaises(VmError) as e:
                vm.wait_for("nothing like this", 0.2)
            self.assertIn("Aember Init System", str(e.exception))

    def test_timeout_scale(self):
        with Vm(self.cmd, self.tmp / "c.log", timeout_scale=0.01) as vm:
            with self.assertRaises(VmError) as e:
                vm.wait_for("nothing", 10)
            self.assertIn("within 0 s", str(e.exception))


class ScenarioTest(unittest.TestCase):
    def test_registry_and_selection(self):
        self.assertEqual(
            list(SCENARIOS)[:6],
            [
                "boot",
                "debug-shell",
                "service-restart",
                "container-restart",
                "poweroff",
                "reboot",
            ],
        )
        self.assertEqual(select(None), list(SCENARIOS))
        self.assertEqual(select(["reboot", "boot"]), ["boot", "reboot"])
        with self.assertRaises(ValueError):
            select(["nope"])
        with self.assertRaises(ValueError):
            scenario("boot", "twice")(lambda s: None)

    def test_configs_from_the_package(self):
        tmp = Path(tempfile.mkdtemp())
        etc = tmp / "overlay_root" / "etc" / "aember"
        etc.mkdir(parents=True)
        (etc / "services.json").write_text('{"processes": [{"name": "a"}]}')
        system = System(QEMU_PROFILES["x64-poky-linux"], tmp, tmp)
        self.assertEqual(system.services(), [{"name": "a"}])
        self.assertEqual(system.containers(), [])

    def test_scenarios_from_another_repository(self):
        tmp = Path(tempfile.mkdtemp())
        (tmp / "theirs.py").write_text(
            textwrap.dedent(
                """
                from aember.steps.testing.system.scenarios import scenario

                @scenario("their-test", "theirs")
                def their_test(system):
                    system.input("server")
                """
            )
        )
        try:
            added = load_scenarios([tmp / "theirs.py"])
            self.assertEqual(added, ["their-test"])
            # Without -k only theirs run; -k still picks any.
            self.assertEqual(select(None, added), ["their-test"])
            self.assertEqual(select(["boot"], added), ["boot"])
            system = System(
                QEMU_PROFILES["x64-poky-linux"], tmp, tmp, name="their-test"
            )
            with self.assertRaisesRegex(AssertionError, "--input server="):
                SCENARIOS["their-test"].run(system)
            system.inputs = {"server": "x"}
            SCENARIOS["their-test"].run(system)
        finally:
            SCENARIOS.pop("their-test", None)
        with self.assertRaises(FileNotFoundError):
            load_scenarios([tmp / "missing.py"])

    def test_inputs(self):
        self.assertEqual(
            parse_inputs(["a=1", "b=x=y", "c="]), {"a": "1", "b": "x=y", "c": ""}
        )
        for bad in ("a", "=1"):
            with self.assertRaises(ValueError):
                parse_inputs([bad])

    def test_image_with_containers(self):
        tmp = Path(tempfile.mkdtemp())
        etc = tmp / "overlay_root" / "etc" / "aember"
        etc.mkdir(parents=True)
        (etc / "containers.json").write_text(
            json.dumps(
                {
                    "allow_unsigned": True,
                    "bridges": [{"name": "lxcbr0"}],
                    "containers": [{"name": "echo"}, {"name": "auth", "old": 1}],
                }
            )
        )
        # A bundle of today's aember-build-helper: the package itself.
        bundle = tmp / "auth"
        bundle.mkdir()
        (bundle / "auth.aember").write_bytes(b"package")
        (bundle / "container.json").write_text(
            '{"name": "auth", "dependencies": ["echo"]}'
        )
        # One of the layout before packages: made into a package.
        old = tmp / "agent"
        files = old / "overlay" / "var" / "lib" / "aember" / "agent"
        files.mkdir(parents=True)
        (files / "agent.squashfs").write_bytes(b"hsqs-img")
        (files / "lxc.container.conf").write_text(
            "lxc.rootfs.path = dir:/somewhere\nlxc.uts.name = agent\n"
        )
        (old / "container.json").write_text(
            json.dumps(
                {
                    "name": "agent",
                    "container": {"args": ["/usr/bin/agent"]},
                    "restart_policy": "always",
                }
            )
        )
        system = System(QEMU_PROFILES["x64-poky-linux"], tmp, tmp / "log")
        with mock.patch.object(System, "image_with", return_value=Path("/i")) as w:
            image = system.image_with_containers(
                [bundle, old], {"/etc/aember/auth/auth.json": "{}", "key": b"k"}, "x"
            )
        self.assertEqual(image, Path("/i"))
        tree, label = w.call_args.args
        self.assertEqual(label, "x")
        merged = json.loads((tree / "etc/aember/containers.json").read_text())
        self.assertEqual(merged["bridges"], [{"name": "lxcbr0"}])
        self.assertTrue(merged["allow_unsigned"])
        # Same name replaced, others kept, new ones last.
        self.assertEqual(
            merged["containers"],
            [
                {"name": "echo"},
                {"name": "auth", "dependencies": ["echo"]},
                {"name": "agent"},
            ],
        )
        packages = tree / "var/lib/aember/packages"
        self.assertEqual((packages / "auth.aember").read_bytes(), b"package")
        out = tmp / "unpacked"
        config = aember_package.extract(packages / "agent.aember", out)
        self.assertEqual(
            config,
            {
                "name": "agent",
                "command": ["/usr/bin/agent"],
                "restart_policy": "always",
            },
        )
        self.assertIn(
            "lxc.rootfs.path = dir:/var/lib/aember/containers/agent",
            (out / "lxc.container.conf").read_text(),
        )
        self.assertEqual((out / "image.squashfs").read_bytes(), b"hsqs-img")
        self.assertEqual((tree / "etc/aember/auth/auth.json").read_text(), "{}")
        self.assertEqual((tree / "key").read_bytes(), b"k")

    def test_container_config_from_the_package(self):
        tmp = Path(tempfile.mkdtemp())
        d = tmp / "overlay_root" / "var" / "lib" / "aember" / "echo"
        d.mkdir(parents=True)
        (d / "config.json").write_text('{"name": "echo", "command": ["/x"]}')
        system = System(QEMU_PROFILES["x64-poky-linux"], tmp, tmp)
        self.assertEqual(system.container_config("echo")["command"], ["/x"])
        self.assertEqual(system.container_config("missing"), {})

    def test_configs_from_the_image_without_overlay_root(self):
        tmp = Path(tempfile.mkdtemp())
        system = System(QEMU_PROFILES["x64-poky-linux"], tmp, tmp)
        with mock.patch.object(
            System, "image_file", return_value='{"containers": [{"name": "c"}]}'
        ) as f:
            self.assertEqual(system.containers(), [{"name": "c"}])
        f.assert_called_once_with("/etc/aember/containers.json")

    def test_container_app(self):
        c = {"command": ["/bin/sh", "/echo-alpine.sh"]}
        self.assertEqual(_container_app(c), "aember-init /bin/sh /echo-alpine.sh")

    def test_boot_timeline_in_order_of_since(self):
        status = {
            "services": [
                {"name": "b", "state": "running", "since_ms": 3000},
                {"name": "a", "state": "running", "since_ms": 1200},
                {"name": "old", "state": "running"},  # an older aember: no since
            ]
        }
        self.assertEqual(
            boot_timeline(status), [(1200, "a", "running"), (3000, "b", "running")]
        )
        self.assertEqual(boot_timeline({}), [])

    def test_summary(self):
        md = summary_markdown([("boot", True, 4.2, ""), ("reboot", False, 30, "x")])
        self.assertIn("| boot | pass | 4 s |", md)
        self.assertIn("| reboot | **FAIL** | 30 s |", md)


if __name__ == "__main__":
    unittest.main()
