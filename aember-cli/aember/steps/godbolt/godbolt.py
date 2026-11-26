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
import re
import shlex
import subprocess
import time
import urllib.request
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
MAX_HEADER_BYTES = 1024 * 1024

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
class Library:
    """aember's static libraries for one target, as a CE library: picked in
    CE's "Libraries" menu, they are linked AFTER the compiled source (user
    options come before it, so `-l` typed there never resolves anything)."""

    include: str
    libpath: list[str]
    links: list[str]


@dataclass(frozen=True)
class Compiler:
    """One CE compiler entry: an SDK g++ with aember's flags for one target."""

    arch: str
    exe: str
    options: list[str]
    library: Library | None = None

    @property
    def id(self) -> str:
        return "aember-" + self.arch.replace("-poky-linux", "").replace("-", "_")

    @property
    def library_id(self) -> str:
        """CE library ids allow no dashes in the properties key."""
        return self.id.replace("-", "_") + "_libs"

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


# Third-party targets aember's libraries link against -> linker names, in
# link order (spdlog needs fmt). Header-only ones link nothing.
EXTERNAL_LINKS = {
    "spdlog::spdlog": ["spdlog", "fmt"],
    "fmt::fmt": ["fmt"],
    "LibLZMA::LibLZMA": ["lzma"],
    "nlohmann_json::nlohmann_json": [],
    "Threads::Threads": [],
}

_ADD = re.compile(r"add_library\((\S+) (?:STATIC|INTERFACE) IMPORTED\)")
_PROPS = re.compile(r"set_target_properties\((\S+) PROPERTIES(.*?)\n\s*\)", re.S)
_DEPS = re.compile(r'INTERFACE_LINK_LIBRARIES "([^"]*)"')
_LOCATION = re.compile(r'IMPORTED_LOCATION_\w+ "[^"]*/lib(?P<name>[^"/]+)\.a"')
_GENEX = re.compile(r"^\\?\$<LINK_ONLY:(.*)>$")


def link_order(cmake_dir: Path) -> list[str]:
    """The `-l` names of aember's installed static libraries in link order
    (each before the ones it needs), then the third-party ones, from the
    CMake export files under lib/cmake/aember-libs/. A library whose export
    has no .a (an INTERFACE target) links nothing itself but passes its
    dependencies on."""
    deps: dict[str, list[str]] = {}
    files: dict[str, str] = {}
    for f in sorted(cmake_dir.rglob("*.cmake")):
        text = f.read_text()
        for name in _ADD.findall(text):
            deps.setdefault(name, [])
        for name, body in _PROPS.findall(text):
            if m := _DEPS.search(body):
                for dep in m.group(1).split(";"):
                    dep = _GENEX.sub(r"\1", dep.strip())
                    if dep:
                        deps.setdefault(name, []).append(dep)
            if m := _LOCATION.search(body):
                files[name] = m.group("name")

    order: list[str] = []  # dependencies first (post-order)
    seen: set[str] = set()

    def visit(target: str) -> None:
        if target in seen:
            return
        seen.add(target)
        for dep in deps.get(target, []):
            if dep in deps:
                visit(dep)
        order.append(target)

    for target in sorted(deps):
        visit(target)

    links = [files[t] for t in reversed(order) if t in files]
    external: list[str] = []
    for target in reversed(order):
        for dep in deps.get(target, []):
            if dep in deps:
                continue
            names = EXTERNAL_LINKS.get(dep)
            if names is None:
                if "::" in dep:
                    log.debug("godbolt: no link name for %s", dep)
                    continue
                names = [dep]
            external += [n for n in names if n not in external]
    return links + external


def find_library(root: Path, arch: str, build_type: str) -> Library | None:
    """aember's installed libraries of one target (build/install/<arch>/
    <type>/lib) plus vcpkg's, at the paths Compiler Explorer sees them."""
    install = Path("build") / "install" / arch / build_type
    cmake_dir = root / install / "lib" / "cmake" / "aember-libs"
    if not cmake_dir.is_dir():
        return None
    links = link_order(cmake_dir)
    if not links:
        return None
    vcpkg = Path("build") / "configure" / arch / build_type / "vcpkg_installed"
    vcpkg = vcpkg / arch / "lib"
    libpath = [f"{CONTAINER_ROOT}/{install}/lib"]
    if (root / vcpkg).is_dir():
        libpath.append(f"{CONTAINER_ROOT}/{vcpkg}")
    return Library(f"{CONTAINER_ROOT}/{install}/include", libpath, links)


def find_compilers(root: Path, build_type: str) -> list[Compiler]:
    """A compiler per target that has a configured build (compile_commands.json
    under build/configure/<arch>/<build_type>/): x64 first (CE's default),
    glibc before musl. Each gets its target's installed libraries, if built."""
    compilers = []
    for arch in sorted(Architecture, key=_arch_order):
        db = root / "build" / "configure" / arch.value / build_type
        db = db / "compile_commands.json"
        if not db.is_file():
            continue
        exe, options = options_from(json.loads(db.read_text()))
        library = find_library(root, arch.value, build_type)
        compilers.append(Compiler(arch.value, exe, options, library))
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
        ]
        if c.library:
            # Only this target's build of the libraries in its Libraries menu.
            lines.append(f"{p}.supportsLibraries={c.library_id}")
        lines.append("")

    with_libs = [c for c in compilers if c.library]
    if with_libs:
        lines.append("libs=" + ":".join(c.library_id for c in with_libs))
    for c in with_libs:
        lib = c.library
        p = f"libs.{c.library_id}"
        v = f"{p}.versions.build"
        lines += [
            f"{p}.name=aember libraries ({c.arch})",
            f"{p}.description=aember's static libraries from build/install "
            "(linked after your code; Compile to binary)",
            f"{p}.versions=build",
            f"{v}.version=this build",
            f"{v}.path={lib.include}",
            f"{v}.libpath={':'.join(lib.libpath)}",
            f"{v}.staticliblink={':'.join(lib.links)}",
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


def session_state(
    source: str, compiler_id: str, options: str = "", library_id: str = ""
) -> dict:
    """CE's session state: @p source in an editor, @p compiler_id's assembly
    next to it (with aember's libraries selected when @p library_id)."""
    compiler: dict = {"id": compiler_id, "options": options}
    if library_id:
        compiler["libs"] = [{"id": library_id, "ver": "build"}]
    return {
        "sessions": [
            {"id": 1, "language": "c++", "source": source, "compilers": [compiler]}
        ]
    }


def client_state_url(base: str, state: dict) -> str:
    """The whole state in the link (CE's /clientstate/<base64 JSON> route).
    Long: a 20 KB source makes a ~22 KB link, which is why the server runs
    with a 1 MiB header limit; short_link() is the normal way."""
    encoded = base64.urlsafe_b64encode(json.dumps(state).encode()).decode()
    return f"{base.rstrip('/')}/clientstate/{encoded}"


def short_link(
    base: str, state: dict, opener=urllib.request.urlopen, public: str = ""
) -> str:
    """A /z/<id> link under @p public (default @p base): the state stored
    through CE's shortener (POST @p base/api/shortener; kept in the
    container's /tmp until it stops)."""
    req = urllib.request.Request(
        f"{base.rstrip('/')}/api/shortener",
        json.dumps(state).encode(),
        {"Content-Type": "application/json", "Accept": "application/json"},
    )
    with opener(req, timeout=30) as reply:
        url = json.load(reply)["url"]
    # CE answers with the host the request named; print ours.
    link_id = url.rstrip("/").rsplit("/", 1)[1]
    return f"{(public or base).rstrip('/')}/z/{link_id}"


def file_link(
    base: str, state: dict, opener=urllib.request.urlopen, public: str = ""
) -> str:
    """short_link(), or the long link when the shortener can't be reached
    (e.g. a dev container that doesn't see the host's localhost)."""
    try:
        return short_link(base, state, opener, public)
    except (OSError, ValueError, KeyError, IndexError) as e:
        log.warning("Compiler Explorer's shortener failed (%s): long link", e)
        return client_state_url(public or base, state)


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
    # node's default 16 KiB header limit answers 431 to a long /clientstate/
    # link (a whole source file in the URL).
    cmd += ["-e", f"NODE_OPTIONS=--max-http-header-size={MAX_HEADER_BYTES}"]
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
