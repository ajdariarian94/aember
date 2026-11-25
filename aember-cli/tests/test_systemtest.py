"""The system-test harness (`aember systemtest`) without QEMU: the console,
the QEMU command line, and the debug-shell protocol against a fake device
(a Python script that answers like aember's console)."""

import json
import sys
import tempfile
import textwrap
import threading
import unittest
from pathlib import Path

from aember.steps.launching.launch import QEMU_PROFILES
from aember.steps.testing.system.scenarios import (
    SCENARIOS,
    System,
    _container_app,
    scenario,
)
from aember.steps.testing.system.step import select, summary_markdown
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

    def test_accel(self):
        self.assertEqual(accel_args(True), ["-accel", "kvm"])
        self.assertEqual(accel_args(False), [])


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

    def test_container_app(self):
        c = {"container": {"args": ["/bin/sh", "/echo-alpine.sh"]}}
        self.assertEqual(_container_app(c), "aember-init /bin/sh /echo-alpine.sh")

    def test_summary(self):
        md = summary_markdown([("boot", True, 4.2, ""), ("reboot", False, 30, "x")])
        self.assertIn("| boot | pass | 4 s |", md)
        self.assertIn("| reboot | **FAIL** | 30 s |", md)


if __name__ == "__main__":
    unittest.main()
