# AGENTS.md

Guidance for AI coding agents (Claude Code, Copilot, Codex, …) working in
**aember**.

## What this repo is

**Aember** is a PID 1 (init system) for constrained embedded Linux, written in
modern C++ (**C++23**). It supervises processes, runs LXC containers, loads
kernel modules, brings up the network and pivots from the initramfs to the
real root filesystem. This repo holds:

- the C++ code: `libraries/` (the init system as static libs), `executables/`
  (the `aember` binary), `examples/`
- the `aember` CLI (`aember-cli/`, plain files in this repo), which fetches
  the prebuilt resources, configures, builds, packs, launches in QEMU and
  releases the system
- the dev container, toolchain files, vcpkg setup and the rootfs overlay
  (`resources/`)

Kernel, BusyBox, LXC and the Poky SDK are **not built here**. They come
prebuilt from the sibling repo **aember-resources**, see below.

### Boot flow

```
kernel ─► initramfs /init ─► usr/bin/aember            AemberInit::StartInitramfs()
             mounts, loads modules, mounts the real root, pivot_root
          ─► real root: aember --root                   AemberInit::StartRoot()
             services (ProcessManager), LXC containers (ContainerManager),
             network, heartbeat/monitor
```

`executables/aember/main.cpp` picks the mode with CLI11 (`--root`).

## aember-resources (kernel, BusyBox, LXC, SDK)

aember-resources (`../aember-resources`,
<https://github.com/ajdariarian94/aember-resources>) builds the low-level
pieces with kas/Yocto and publishes them as assets of the GitHub release
`v<version>`. The version aember uses is pinned in
`aember-cli/aember/config/resources.py` (`RESOURCES_VERSION`, now `0.1.1`;
x64 only so far).

`aember fetch-resources` (also the first step of `aember qemu build`,
`steps/resources/fetch_resources.py`):

1. downloads the release's `manifest.json` over plain HTTPS (the repo is
   public: no `gh`, no token),
2. for each asset of the architecture, skips it if
   `build/aember-resources/<arch>/<dir>/.asset-sha256` already holds the
   manifest's sha256, otherwise downloads it, verifies the sha256 and
   installs it: zips are extracted (Unix modes restored), the SDK installer
   runs with `-d`.

| Component | Asset                                  | Installed into `build/aember-resources/<arch>/` |
|-----------|----------------------------------------|-------------------------------------------------|
| kernel    | `aember-kernel-<arch>-v<V>.zip`        | `kernel/` (`bzImage`/`Image`, `lib/modules/<kver>/`) |
| busybox   | `aember-busybox-<arch>-v<V>.zip`       | `busybox/busybox` (static)                      |
| lxc       | `aember-lxc-<arch>-v<V>.zip`           | `virtualization/` (`libs/liblxc.so*`, `include/`, `pkgconfig/lxc.pc`) |
| sdk       | `aember-sdk-<arch>-v<V>.sh` (installer) | `sdk/` (GCC 15, sysroots, `environment-setup-*`) |

- **The SDK bakes its install path in.** It's installed at
  `/aember_ws/aember/build/aember-resources/<arch>/sdk` inside the dev
  container. Moving it means deleting `sdk/` and fetching again.
- To use a newer aember-resources release, bump `RESOURCES_VERSION`; the
  sha256 markers make the next fetch replace what changed.
- To test an aember-resources release **before it's published**, copy its
  `build/release/v<V>/` files (without `.meta/`) somewhere under this repo
  and set `AEMBER_RESOURCES_URL=file:///aember_ws/aember/<that dir>`; the
  fetcher then reads `manifest.json` and the assets from there.
- Asset names and `manifest.json` are an interface owned by aember-resources
  (see its AGENTS.md). Don't edit `../aember-resources` from here.
- The SDK has no `pkg-config`; Poky expects the host's (`pkgconf` is in the
  dev image). `lxc.pc` is relocatable (`${pcfiledir}`).

## Repository layout

```
.devcontainer/          Ubuntu 24.04 dev image (prebuilt ghcr.io/ajdariarian94/aember-dev)
aember-cli/             the `aember` CLI (Python, Typer): cli/, steps/, config/, utils/
CMakeLists.txt          Superbuild: ExternalProject libraries → executables, examples
                        project VERSION here is the app version (CLI reads it)
libraries/              aember-libs: one static lib per dir, + utils/<x>
executables/aember/     main.cpp → the `aember` binary (PID 1)
examples/               demo binaries packed into the initramfs
resources/<arch>/rootfs rootfs overlay: configs tracked, *.squashfs images from the release
tools/toolchains/       <arch>.cmake, chain-loaded by vcpkg (SDK compilers, sysroot, flags)
tools/scripts/          init.sh (postStart), prerequisites.sh, bashrc.d/
vcpkg/                  SUBMODULE (fork) incl. x64/aarch64-poky-linux triplets
vcpkg.json              cli11, gtest, nlohmann-json, spdlog
doc/                    Doxyfile, changelog/ (written by `aember changelog`)
build/                  git-ignored: aember-resources/, configure/, install/,
                        package/, downloads/, logging/, release/
```

Checkouts from before the move to aember-resources may still have:
- a `yocto/` dir (old in-repo Yocto output, several GB): unused, delete it;
- a `build/configure/<arch>/<type>/` with the gcc-reflection compiler cached
  in its sub-project `CMakeCache.txt` files (symptom: "LTO version 15.1
  instead of the expected 16.0" at link time): delete that whole dir (not
  just the caches, or ExternalProject skips its configure) and build again.

## C++ libraries

All live in namespace `aember::<lib_name>` (dashes → underscores), headers
under `include/aember-libs/<lib>/`, CMake target `aember-libs::<lib>`.

| Library             | Role |
|---------------------|------|
| `aember-init`       | Top level: `StartInitramfs()` / `StartRoot()`, wires everything together |
| `root-manager`      | Mounts the real root and does `pivot_root` |
| `mount-manager`     | Mounts and tracks filesystems |
| `module-loader`     | Loads kernel modules from `modules.json` |
| `network-manager`   | Interfaces, bridge, connectivity monitoring (`network.json`) |
| `process-manager`   | Spawns and tracks native processes |
| `container-manager` | LXC container lifecycle (liblxc from `VIRT_ROOT`, set by the toolchain file) |
| `child-supervisor`  | Reaps children, dispatches PID exit events |
| `service-manager`   | Coordinator over process/container managers, dependency order |
| `device-health`     | Heartbeat + `AemberMonitor` runtime stats |
| `utils/*`           | config, container, logging (spdlog), module, mount, network, process, root, shell, signal (signalfd + epoll) |

Runtime config lives in the overlay at
`resources/<arch>/rootfs/etc/aember/{services,containers,modules,network}.json`;
container images under `rootfs/var/lib/aember/<name>/`.

The overlay's text files (the JSON configs, `lxc.container.conf`, the udhcpc
script) are **tracked in git**; edit them here and commit. The container
images (`*.squashfs`, e.g. the Qt-based `aember-monitor`) are git-ignored and
come from aember's own release `v<project VERSION>` as
`aember-resources-v<V>.tar.gz` (public, plain HTTPS). `CheckResources`
(`utils/check_resources.py`) downloads it when no image is present and
extracts only files that don't exist yet, so tracked configs always win.
`aember release` packs the whole overlay into that tarball.

## From a fresh clone (x64)

```bash
git clone https://github.com/ajdariarian94/aember.git   # inside the dev container
aember app build      # images, aember-resources (kernel/BusyBox/LXC/SDK), vcpkg, configure, build
aember app pack       # initramfs + rootfs image in build/package/x64-poky-linux/Release/
aember app launch     # QEMU in xterm; --headless --timeout 30 for a terminal-only check
```

`aember app build` = `CheckResources` → `FetchResources` → `ConfigureStep`
(initialises the `vcpkg` submodule if a plain clone left it empty) →
`BuildStep`. Every step skips work that's already done, so it's also the
everyday build command.

## Build pipeline (`aember qemu build`)

`aember-cli/aember/cli/qemu/build.py` runs, in order:

1. `CleanStep`, `CheckResources`: fetch the overlay images (above)
   (`aember-resources-v<V>.tar.gz` from *aember's own* release; unrelated
   to the aember-resources repo despite the name)
2. `FetchResources`: kernel, BusyBox, LXC, SDK from aember-resources (above)
3. `ConfigureStep`: sources the SDK `environment-setup-*`, unsets CC/CXX,
   runs CMake with the vcpkg toolchain chain-loading `tools/toolchains/<arch>.cmake`
   into `build/configure/<arch>/<BuildType>/`
4. `BuildStep`: `cmake --build`, then merges `compile_commands.json` for clangd
5. `PackStep` (`steps/packaging/`): initramfs tree + BusyBox (installed with
   `qemu-<arch>-static` when cross) + `aember` + examples/tests + `/init`
   symlink; runtime libs resolved with `objdump -p` (NEEDED) from the LXC
   libs and the SDK target sysroot; kernel modules from `modules.json` +
   `depmod`; `aember-initramfs.cpio.gz`; `aember-rootfs.img` built with
   guestfish in a `docker run` of the current dev image (bind mount via
   `HOST_ROOT_PATH`)

Every name in `resources/<arch>/rootfs/etc/aember/modules.json` must exist in
the aember-resources kernel, as a `.ko` or listed in `modules.builtin`
(bridge, veth, br_netfilter, overlay, llc, stp, ttm are built in). The
packer fails on anything missing, packs each listed `.ko` **plus its
dependencies** (from `modules.dep`, generated with `depmod -n` because the
kernel asset doesn't ship one; e.g. virtio-gpu needs drm_shmem_helper), and
copies `modules.builtin*`/`modules.order` into the rootfs. BusyBox `modprobe`
does **not** read `modules.builtin`, so the C++ `ModuleLoader` checks
`/lib/modules/<release>/modules.builtin` itself and skips built-in modules.

`aember qemu launch` boots `build/package/<arch>/<BuildType>/` in
`qemu-system-x86_64` / `qemu-system-aarch64 -machine virt`, in xterm windows
(Debug also starts GDB). **`aember qemu launch --headless --timeout 30`**
runs QEMU in the current terminal with the serial console on stdout, for
checks without a display. aember is PID 1 and never exits, so the timeout is
what ends the run; the boot itself takes ~10 s. Headless keeps the virtual
GPU (`-display none`, not `-vga none`): the `aember-monitor` container binds
`/dev/dri` and `/dev/fb0` and fails without them. Serial logs go to
`build/logging/<arch>/`.

Debugging a container that fails `lxc start`: its LXC log is in the VM's
`/tmp` (tmpfs, gone after the run). Point `lxc.log.file` at `/var/log/` in the
**built** `aember-rootfs.img` (guestfish `download`/`upload` via a `docker
run` of the dev image), boot, then read the log back with guestfish. The next
pack regenerates the image.

Other commands: `aember fetch-resources`, `aember app configure|build|pack|launch`,
`aember lint`, `aember changelog` (bumps `project(VERSION)` in
`CMakeLists.txt` and writes `doc/changelog/`), `aember release` (overlay,
app package and binary as tar.gz into `build/release/<arch>/<type>/`),
`aember generate-docs|view-docs` (Doxygen).

The toolchain files find everything relative to the repo
(`AEMBER_ROOT`, `AEMBER_RESOURCES_DIR`, `POKY_SYSROOT`, `POKY_SDK_ROOT`,
`VIRT_ROOT`). Both C and C++ use the SDK's `<triplet>-gcc`/`-g++`.

## Coding conventions

C++:
- **C++23** (`CMAKE_CXX_STANDARD 23` in the toolchain files, Poky SDK GCC
  15). No C++26 or P2996 reflection: there is no reflection compiler any
  more.
- clang-format: Google base, 80 columns, `PointerAlignment: Left`, short
  blocks/ifs/loops on one line allowed. Include order (enforced): 1.
  `<aember-libs/...>`, 2. third party (`nlohmann`, `spdlog`, `CLI`, `gtest`),
  3. std headers, 4. C system headers.
- Files start with a Doxygen header (`@file`, `@author`, `@brief`,
  `@version`, `@date`, `@copyright`). `#pragma once`.
- Classes `PascalCase`, methods `PascalCase()`, members `snake_case_`.
- A library dir has `CMakeLists.txt` (STATIC lib + `aember-libs::` alias +
  install/export), `Config.cmake.in`, `include/`, `src/`, optional `tests/`
  (GTest, `<lib>-unittest`, `add_test`). Several libraries have their
  `tests` subdirectory commented out; check before assuming tests run.
- Tests build only with `--with-tests`, which forces x64 Debug; the test
  binaries are packed into the initramfs and run inside QEMU.

CMake: cmake-format (`.cmake-format.yaml`) and cmakelint (`.cmakelintrc`).

Python (aember-cli): Typer commands in `cli/`, `Step` subclasses in
`steps/<area>/`, `Pipeline([...]).run(ctx)`, per-arch data in `config/`,
`log = logging.getLogger("aember.<area>")` with `%s` args. Run `aember lint`
(ruff + black + clang-format + cmake-format/cmakelint) before finishing.

## Submodules

Only `vcpkg` (fork, with the poky triplets) is a submodule. Don't bump its
pointer without being asked. `aember-cli/` used to be a submodule
(ajdariarian94/aember-cli); it's now regular files here, so edit it
directly.

## Environment

- Work inside the dev container: the repo is at `/aember_ws/aember`, venv
  `/venv`, privileged, `--net=host`, Docker via the host socket,
  `HOST_ROOT_PATH` = host path of the repo (needed by the guestfish `docker
  run`). `tools/scripts/init.sh` fixes the Docker GID and sets up the shell
  on start.
- `/venv/bin/aember` imports from `aember-cli/` through `PYTHONPATH`, set by
  the container's shell setup. In a non-interactive `docker exec`, pass
  `-e PYTHONPATH=/aember_ws/aember/aember-cli` and put `/venv/bin` on `PATH`.
- `devcontainer.json` uses the prebuilt image; switch to the `build:` block
  to test Dockerfile changes.
- Downloads (overlay images, aember-resources) use plain HTTPS; `gh` and a
  token are not needed to build. For other `gh` use, `GITHUB_TOKEN` can come
  from `tools/scripts/.env` (git-ignored, **never commit or print it**).
- `aember-cli/aember/config/host_path.py` reads `HOST_ROOT_PATH` at import.

## Known issues (don't fix unless asked)

- `cli/qemu/build.py`: `if build_tests or BuildType.debug:` is always true,
  so every `aember qemu build` starts with `CleanStep`.
- aarch64: aember-resources has no aarch64 assets yet, so
  `aember qemu build --architecture aarch64-poky-linux` stops at
  `FetchResources`. The QEMU profile still uses `-cpu cortex-a57`.
- The dev Dockerfile still has Yocto-era leftovers (GNU tar 1.34
  downgrade, gcc-12/14 PPA). Harmless, but can go on the next image rebuild.

## Working agreements for agents

- Branch off `main` (`fix/<x>`, `enhancement/<x>`, `docs/<x>`, …) and merge
  through a PR. Commit only when asked.
- Don't commit unrelated working-tree changes the user has in progress.
- Confirm before deleting or overwriting `resources/` or `build/`
  (especially `build/aember-resources/`): they're expensive to regenerate.
- Don't publish GitHub releases or upload assets unless explicitly asked.
- Don't start long builds without the user asking.
- Keep this file current. Update it in the same change when something here
  goes stale.
