"""
A booted aember in QEMU that a system test drives through the serial
console: wait for console output, open the debug shell, run commands in it
and read their output and exit code, read /var/log/aember-status.json.

Every wait is for a condition with a deadline (scaled by `timeout_scale`
for slow machines, e.g. TCG on a CI runner), never a fixed sleep.
"""

from __future__ import annotations

import json
import logging
import os
import re
import subprocess
import threading
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import TypeVar

from aember.steps.launching.launch import QemuProfile, headless_profile

log = logging.getLogger("aember.systemtest")

T = TypeVar("T")

# What PID 1 prints when init is up (aember-init.cpp).
READY = r"Aember Init System started successfully"
# BusyBox ash's prompt; it never ends its line.
PROMPT = r"# $"
SHELL_EXITED = r"Debug shell exited"

# Escape sequences (colours, cursor moves) and a sequence cut off at the end
# of a chunk (kept for the next one).
_ESCAPE = re.compile(r"\x1b(\[[0-9;?]*[A-Za-z]|\][^\x07]*\x07|[cM])")
_PARTIAL_ESCAPE = re.compile(r"\x1b(\[[0-9;?]*|\][^\x07]*)?$")


class VmError(AssertionError):
    """A system-test expectation failed (an AssertionError: a test failure,
    not a crash of the harness). Carries the end of the console."""


class Console:
    """The serial console's text so far (escape sequences removed, \\r\\n ->
    \\n), with waits for a pattern. Thread-safe: QEMU's output arrives on a
    reader thread."""

    def __init__(self) -> None:
        self._text = ""
        self._carry = ""
        self._closed = False
        self._cond = threading.Condition()

    def add(self, chunk: str) -> None:
        data = self._carry + chunk
        partial = _PARTIAL_ESCAPE.search(data)
        self._carry = data[partial.start() :] if partial else ""
        if partial:
            data = data[: partial.start()]
        clean = _ESCAPE.sub("", data).replace("\r\n", "\n").replace("\r", "")
        with self._cond:
            self._text += clean
            self._cond.notify_all()

    def close(self) -> None:
        with self._cond:
            self._closed = True
            self._cond.notify_all()

    @property
    def text(self) -> str:
        with self._cond:
            return self._text

    def mark(self) -> int:
        """Position to search from: only output after this counts."""
        return len(self.text)

    def tail(self, chars: int = 3000) -> str:
        return self.text[-chars:]

    def wait_for(
        self, pattern: str, timeout: float, start: int = 0
    ) -> re.Match[str] | None:
        """First match of @p pattern in the output from @p start on, or None
        after @p timeout seconds or once the console is closed (QEMU gone)."""
        regex = re.compile(pattern, re.MULTILINE)
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                match = regex.search(self._text, start)
                if match:
                    return match
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self._closed:
                    return None
                self._cond.wait(min(remaining, 0.5))


# QEMU binary -> the host machine (uname -m) KVM can run it on.
KVM_HOSTS = {"qemu-system-x86_64": "x86_64", "qemu-system-aarch64": "aarch64"}


def accel_args(
    kvm: bool | None = None, binary: str = "qemu-system-x86_64"
) -> list[str]:
    """`-accel kvm` when /dev/kvm is usable and @p binary emulates the host's
    own architecture (auto when @p kvm is None), else nothing: QEMU's TCG,
    much slower but everywhere."""
    if kvm is None:
        kvm = (
            os.access("/dev/kvm", os.R_OK | os.W_OK)
            and KVM_HOSTS.get(Path(binary).name) == os.uname().machine
        )
    return ["-accel", "kvm"] if kvm else []


def vm_command(
    profile: QemuProfile,
    package_dir: Path,
    append: str = "",
    extra_args: Sequence[str] = (),
    kvm: bool | None = None,
    rootfs: Path | None = None,
    snapshot: bool = True,
) -> list[str]:
    """
    The QEMU command line of a system test: headless like `aember app launch
    --headless`, plus
    - `-snapshot`: writes go to a temporary overlay, the packed image stays
      as it was (every test boots the same image). Without it (@p snapshot
      False) the disk keeps what the run wrote, for tests that boot the same
      disk again (power loss); give such a test its own copy (@p rootfs).
    - `-no-reboot`: a reboot ends QEMU like a power-off, so tests see both;
    - serial console on stdin/stdout (pipes).
    """
    cmd = headless_profile(profile).base_cmd(
        str(package_dir / "kernel"),
        str(package_dir / "aember-initramfs.cpio.gz"),
        str(rootfs or package_dir / "aember-rootfs.img"),
        append,
    )
    cmd += ["-snapshot"] if snapshot else []
    cmd += ["-no-reboot", "-serial", "stdio", "-monitor", "none"]
    cmd += accel_args(kvm, profile.binary)
    cmd += list(extra_args)
    return cmd


class Vm:
    """
    One QEMU run of the packed system. Use as a context manager: QEMU is
    killed on the way out if it still runs, and the whole console goes to
    @p log_path.

        with Vm(cmd, log_path) as vm:
            vm.wait_ready()
            vm.open_shell()
            assert vm.sh("cat /proc/1/comm") == "aember"
    """

    def __init__(
        self, cmd: Sequence[str], log_path: Path, timeout_scale: float = 1.0
    ) -> None:
        self.cmd = list(cmd)
        self.log_path = log_path
        self.scale = timeout_scale
        self.console = Console()
        self.proc: subprocess.Popen[bytes] | None = None
        self._reader: threading.Thread | None = None
        self._marker = 0

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self) -> Vm:
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()

    def start(self) -> None:
        log.debug("QEMU: %s", " ".join(self.cmd))
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.proc = subprocess.Popen(
            self.cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self) -> None:
        assert self.proc and self.proc.stdout
        fd = self.proc.stdout.fileno()
        with open(self.log_path, "wb") as raw:
            while chunk := os.read(fd, 4096):
                raw.write(chunk)
                raw.flush()
                self.console.add(chunk.decode("utf-8", errors="replace"))
        self.console.close()

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self._reader:
            self._reader.join(timeout=5)
        if self.proc:
            for pipe in (self.proc.stdin, self.proc.stdout):
                if pipe:
                    pipe.close()

    # -- console -----------------------------------------------------------

    def fail(self, message: str) -> VmError:
        return VmError(f"{message}\n--- console (end) ---\n{self.console.tail()}")

    def send(self, text: str) -> None:
        assert self.proc and self.proc.stdin
        try:
            self.proc.stdin.write(text.encode())
            self.proc.stdin.flush()
        except BrokenPipeError as e:
            raise self.fail(f"QEMU is gone, can't type {text!r}") from e

    def wait_for(
        self, pattern: str, timeout: float, start: int = 0, what: str = ""
    ) -> re.Match[str]:
        """Wait for @p pattern on the console (from position @p start)."""
        match = self.console.wait_for(pattern, timeout * self.scale, start)
        if not match:
            raise self.fail(
                f"no {what or repr(pattern)} on the console "
                f"within {timeout * self.scale:.0f} s"
            )
        return match

    def wait_ready(self, timeout: float = 60) -> None:
        self.wait_for(READY, timeout, what="'init ready'")

    # -- debug shell -------------------------------------------------------

    def open_shell(self, timeout: float = 10, tries: int = 3) -> None:
        """Press d until the debug shell's prompt shows, then keep kernel
        messages off the console (they'd mix into command output)."""
        for _ in range(tries):
            start = self.console.mark()
            self.send("d")
            if self.console.wait_for(PROMPT, timeout * self.scale, start):
                break
        else:
            raise self.fail("no debug shell prompt after pressing d")
        self.run("dmesg -n 1")  # best effort

    def close_shell(self, timeout: float = 10) -> None:
        start = self.console.mark()
        self.send("exit\n")
        self.wait_for(SHELL_EXITED, timeout, start, what="'debug shell exited'")

    def run(self, command: str, timeout: float = 30) -> tuple[int, str]:
        """Run @p command in the open debug shell: (exit code, output).
        The markers are split with "" so the shell's echo of the command
        line never matches them, only what echo prints."""
        self._marker += 1
        n = self._marker
        start = self.console.mark()
        self.send(f'echo @@B""{n}@@; {command}; echo @@E""{n}:$?@@\n')
        # Anything up to the line end belongs to the marker's line: a kernel
        # message can land there before `dmesg -n 1` took effect.
        begin = self.wait_for(
            rf"^@@B{n}@@[^\n]*\n", timeout, start, what=f"'{command}'"
        )
        end = self.wait_for(
            rf"@@E{n}:(\d+)@@", timeout, begin.end(), what=f"end of '{command}'"
        )
        return int(end.group(1)), self.console.text[begin.end() : end.start()]

    def sh(self, command: str, timeout: float = 30) -> str:
        """Like run(), but a non-zero exit code fails the test. Output
        without the trailing newline."""
        code, out = self.run(command, timeout)
        if code != 0:
            raise self.fail(f"'{command}' exited {code}: {out}")
        return out.rstrip("\n")

    # -- device state ------------------------------------------------------

    def status(self) -> dict:
        """/var/log/aember-status.json (PID 1's heartbeat), parsed."""
        out = self.sh("cat /var/log/aember-status.json")
        try:
            return json.loads(out)
        except json.JSONDecodeError as e:
            raise self.fail(f"aember-status.json is no JSON ({e}): {out}") from e

    def states(self) -> dict[str, str]:
        """name -> state of every service and container in the status."""
        return {s["name"]: s["state"] for s in self.status().get("services", [])}

    def pids(self, text: str) -> list[int]:
        """PIDs of the processes whose `ps` line contains @p text (BusyBox
        ps prints whole command lines)."""
        _, out = self.run(f"ps | grep -F -- '{text}' | grep -v grep")
        return [int(line.split()[0]) for line in out.splitlines() if line.strip()]

    def wait_until(
        self,
        check: Callable[[], T],
        timeout: float,
        what: str,
        interval: float = 1.0,
    ) -> T:
        """Call @p check until it returns something truthy (returned)."""
        deadline = time.monotonic() + timeout * self.scale
        last: T = check()
        while not last:
            if time.monotonic() > deadline:
                raise self.fail(
                    f"{what}: not within {timeout * self.scale:.0f} s "
                    f"(last: {last!r})"
                )
            time.sleep(interval)
            last = check()
        return last

    def wait_exit(self, timeout: float) -> int:
        """Wait for QEMU to end by itself (power-off, reboot): its exit
        code."""
        assert self.proc
        try:
            return self.proc.wait(timeout * self.scale)
        except subprocess.TimeoutExpired as e:
            raise self.fail(
                f"QEMU still runs {timeout * self.scale:.0f} s later"
            ) from e
