"""`aember godbolt`: a local Compiler Explorer with the aember SDK's cross
compilers and aember's own build flags.

Compiler Explorer is upstream's (tools/godbolt/Dockerfile, a pinned commit),
run in Docker with the repo mounted read-only at /aember_ws/aember, the path
the SDKs and compile_commands.json were made for. This module only writes its
two config files (build/godbolt/) and starts or stops the container.
"""

from __future__ import annotations

import base64
import json
import logging
import shlex
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from aember.config.architecture import Architecture

log = logging.getLogger("aember.godbolt")

CONTAINER = "aember-godbolt"
# Keep in step with CE_COMMIT in tools/godbolt/Dockerfile.
IMAGE = "aember-godbolt:gh-20105"
DOCKERFILE_DIR = Path("tools/godbolt")
CONTAINER_ROOT = "/aember_ws/aember"
DEFAULT_PORT = 10240
# The CE source file name compile_commands entries are matched against for
# the flags (a PID 1 library; any entry would do, they share the flags).
FLAGS_FROM = "libraries/process-manager/src/process-manager.cpp"
# Compiling with aember's headers (spdlog, fmt, nlohmann) takes a few seconds,
# more than CE's 7.5 s default on a cold cache.
COMPILE_TIMEOUT_MS = 60000

# CE's instruction set names (asm documentation, opcode tooltips).
INSTRUCTION_SET = {
    "x64": "amd64",
    "aarch64": "aarch64",
    "arm": "arm32",
    "riscv64": "riscv64",
}

# Build-only flags with no meaning in Compiler Explorer: LTO (CE compiles one
# file to assembly), -Werror (a warning in a pasted snippet shouldn't stop
# the output), -g and -pipe (CE adds its own), dependency files.
_DROP = {"-Werror", "-g", "-pipe", "-c", "-MD", "-MMD"}
_DROP_PREFIX = ("-flto", "-Werror=")
_DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}


@dataclass(frozen=True)
class Compiler:
    """One CE compiler entry: an SDK g++ with aember's flags for one target."""

    arch: str
    exe: str
    options: list[str]

    @property
    def id(self) -> str:
        return "aember-" + self.arch.replace("-poky-linux", "").replace("-", "_")

    @property
    def name(self) -> str:
        return f"aember {self.arch} (SDK g++)"

    @property
    def objdump(self) -> str:
        return self.exe.removesuffix("g++") + "objdump"

    @property
    def demangler(self) -> str:
        return self.exe.removesuffix("g++") + "c++filt"

    @property
    def instruction_set(self) -> str:
        return INSTRUCTION_SET[self.arch.split("-")[0]]


def _arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def options_from(entries: list[dict]) -> tuple[str, list[str]]:
    """The compiler and the flags CE should use, from one compile_commands.json.

    Flags (target, -O, -std, defines, -isystem) come from the FLAGS_FROM
    entry, or the first one; -I directories are the union over all entries,
    so any aember header can be included. Returns (compiler, options).
    """
    if not entries:
        raise ValueError("compile_commands.json has no entries")
    main = next((e for e in entries if e["file"].endswith(FLAGS_FROM)), entries[0])
    args = _arguments(main)
    compiler, rest = args[0], args[1:]
    source = main["file"]

    options: list[str] = []
    includes: list[str] = []
    skip = False
    for arg in rest:
        if skip:
            skip = False
            continue
        if arg in _DROP_WITH_ARG:
            skip = True
            continue
        if arg in _DROP or arg.startswith(_DROP_PREFIX) or arg == source:
            continue
        if arg.startswith("-I"):
            continue  # gathered from all entries below
        options.append(arg)

    for entry in entries:
        for arg in _arguments(entry):
            if arg.startswith("-I") and len(arg) > 2 and arg not in includes:
                includes.append(arg)
    return compiler, sorted(includes) + options


def find_compilers(root: Path, build_type: str) -> list[Compiler]:
    """A compiler per target that has a configured build (compile_commands.json
    under build/configure/<arch>/<build_type>/): x64 first (CE's default),
    glibc before musl."""
    compilers = []
    for arch in sorted(Architecture, key=_arch_order):
        db = root / "build" / "configure" / arch.value / build_type
        db = db / "compile_commands.json"
        if not db.is_file():
            continue
        exe, options = options_from(json.loads(db.read_text()))
        compilers.append(Compiler(arch.value, exe, options))
    return compilers


def _arch_order(arch: Architecture) -> tuple[int, bool]:
    cpu = arch.value.split("-")[0]
    return list(INSTRUCTION_SET).index(cpu), arch.value.endswith("-musl")


def properties(compilers: list[Compiler]) -> str:
    """CE's c++.local.properties: only the aember compilers, x64 first."""
    if not compilers:
        raise ValueError("no compilers")
    ids = [c.id for c in compilers]
    lines = [
        "# Written by `aember godbolt`; regenerated on every start.",
        "compilers=&aember",
        f"defaultCompiler={ids[0]}",
        "group.aember.compilers=" + ":".join(ids),
        "group.aember.groupName=aember SDK",
        "group.aember.compilerType=gcc",
        "group.aember.isSemVer=false",
        # Cross compilers: the output can't run here.
        "group.aember.supportsExecute=false",
        "group.aember.supportsBinary=true",
        "group.aember.objdumperType=binutils",
        "",
    ]
    for c in compilers:
        p = f"compiler.{c.id}"
        lines += [
            f"{p}.exe={c.exe}",
            f"{p}.name={c.name}",
            f"{p}.options={shlex.join(c.options)}",
            f"{p}.objdumper={c.objdump}",
            f"{p}.demangler={c.demangler}",
            f"{p}.instructionSet={c.instruction_set}",
            # -masm=intel only exists for x86.
            f"{p}.intelAsm={'-masm=intel' if c.instruction_set == 'amd64' else ''}",
            f"{p}.supportsIntel={'true' if c.instruction_set == 'amd64' else 'false'}",
            "",
        ]
    return "\n".join(lines)


def ce_properties() -> str:
    """compiler-explorer.local.properties (site-wide settings)."""
    return "\n".join(
        [
            "# Written by `aember godbolt`.",
            f"compileTimeoutMs={COMPILE_TIMEOUT_MS}",
            # Short links are stored here; /ce is read-only for our user.
            "localStorageFolder=/tmp/ce-storage/",
            "",
        ]
    )


def write_config(out: Path, compilers: list[Compiler]) -> tuple[Path, Path]:
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / "c++.local.properties"
    site = out / "compiler-explorer.local.properties"
    cpp.write_text(properties(compilers))
    site.write_text(ce_properties())
    return cpp, site


def client_state_url(
    base: str, source: str, compiler_id: str, options: str = ""
) -> str:
    """A CE link that opens with @p source in an editor and @p compiler_id's
    assembly next to it (CE's /clientstate/<base64 JSON> route)."""
    state = {
        "sessions": [
            {
                "id": 1,
                "language": "c++",
                "source": source,
                "compilers": [{"id": compiler_id, "options": options}],
            }
        ]
    }
    encoded = base64.urlsafe_b64encode(json.dumps(state).encode()).decode()
    return f"{base.rstrip('/')}/clientstate/{encoded}"


def docker_run_command(host_root: str, port: int, uid: int, gid: int) -> list[str]:
    """`docker run` for CE: the repo read-only at the path the SDKs expect,
    the generated config over CE's, only on localhost."""
    config = f"{host_root}/build/godbolt"
    mounts = [
        f"{host_root}:{CONTAINER_ROOT}:ro",
        f"{config}/c++.local.properties:/ce/etc/config/c++.local.properties:ro",
        f"{config}/compiler-explorer.local.properties:"
        "/ce/etc/config/compiler-explorer.local.properties:ro",
    ]
    cmd = ["docker", "run", "-d", "--name", CONTAINER, "--memory", "3g"]
    cmd += ["-u", f"{uid}:{gid}", "-p", f"127.0.0.1:{port}:10240"]
    for m in mounts:
        cmd += ["-v", m]
    return cmd + [IMAGE]


def _docker(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["docker", *args], capture_output=True, text=True, check=check
    )


def image_exists() -> bool:
    return _docker("image", "inspect", IMAGE, check=False).returncode == 0


def build_image(root: Path) -> None:
    log.info("Building %s (upstream Compiler Explorer, a few minutes once)", IMAGE)
    subprocess.run(
        ["docker", "build", "-t", IMAGE, str(root / DOCKERFILE_DIR)], check=True
    )


def stop() -> bool:
    """Remove the container; False if none was there."""
    return _docker("rm", "-f", CONTAINER, check=False).returncode == 0


def wait_ready(timeout: float = 180.0) -> None:
    """Wait for CE's "Listening on" log line (the dev container may not
    reach the host's localhost, the logs always work)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = _docker("inspect", "-f", "{{.State.Running}}", CONTAINER, check=False)
        logs = _docker("logs", CONTAINER, check=False)
        text = logs.stdout + logs.stderr
        if "Listening on" in text:
            return
        if state.stdout.strip() == "false":
            raise RuntimeError(f"Compiler Explorer exited:\n{text[-2000:]}")
        time.sleep(1)
    raise TimeoutError(f"Compiler Explorer not ready after {timeout:.0f} s")
