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
`aember-cli/aember/config/resources.py` (`RESOURCES_VERSION`, now `0.1.3`;
x64, aarch64 and arm32).

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
tools/triplets/         overlay triplets not in the submodule (arm-poky-linux),
                        passed as VCPKG_OVERLAY_TRIPLETS by ConfigureStep
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
| `aember-init`       | Top level: `StartInitramfs()` / `StartRoot()`, wires everything together; `pid1.h`: terminate handler, signal mask, `Finish()` (shutdown) |
| `root-manager`      | Mounts the real root and does `pivot_root` |
| `mount-manager`     | Mounts and tracks filesystems |
| `module-loader`     | Loads kernel modules from `modules.json` |
| `network-manager`   | Interfaces, bridge, connectivity monitoring (`network.json`) |
| `process-manager`   | Spawns and tracks native processes |
| `container-manager` | LXC container lifecycle (liblxc from `VIRT_ROOT`, set by the toolchain file) |
| `child-supervisor`  | Tracks service pids; `ReapZombies()` is PID 1's only reaper (see below) |
| `service-manager`   | Coordinator over process/container managers, dependency order |
| `device-health`     | Heartbeat + `AemberMonitor` runtime stats |
| `utils/*`           | config, container, logging (spdlog), module, mount, network, process, root, shell, signal (signalfd + epoll) |

**Logging:** one spdlog logger for the whole process
(`utils/logging`: `global_logger()`, the spdlog default logger). A
`Logger{"component"}` is only a name: it logs `[component] message`
through the global logger, whose single sink is a thread-safe `dist_sink`
(console, plus the rotating file once `enable_file_logging()` ran). Don't
create spdlog loggers or call `std::print*`/`printf` in the libraries.

**PID 1 rules** (if PID 1 exits or dies, the kernel panics):
- `main()` never returns as PID 1: it ends in `pid1::Finish()`, which
  reboots/powers off/halts when requested and otherwise reaps forever. The
  terminate handler keeps PID 1 alive after an uncaught exception.
- Init phases run through `Guarded()` in `aember-init.cpp` (log and carry
  on); threads (`jthread` loops, signal callbacks, restart timers) catch
  everything. Parsers return errors, never throw.
- Signals: `pid1::BlockInitSignals()` blocks SIGTERM/INT/HUP/USR1/USR2/CHLD
  first thing in `main()`, before any thread exists (a thread with them
  unblocked takes the default action = init dies). fork() children get them
  unblocked via `pthread_atfork`. Signal callbacks only record a request;
  never call `Stop()` from the signal thread (it joins itself).
- Shutdown like BusyBox init: SIGTERM/SIGINT reboot, SIGUSR2 power off,
  SIGUSR1 halt (BusyBox `reboot`/`poweroff`/`halt` work).
- Reaping: never `waitpid(-1)` while services run. `system()`, modprobe,
  liblxc and the debug shell wait for their own children; a blind
  `waitpid(-1)` steals their status (ECHILD). The reaper thread calls
  `ChildSupervisor::ReapZombies()`: zombies found in `/proc`, tracked pids
  at once, untracked ones after 2 s (an orphan nobody waits for).
- Don't hold a manager's mutex while calling another manager or a
  callback that may call back: they lock it again (std::mutex, not
  recursive). Both `ServiceManager::StopInternal` (container stop) and
  `StartInternal` (dependency check → `GetState`) deadlocked that way.
- One bad `services.json` entry (dependency cycle, unknown dependency)
  only skips that entry; the rest still starts.
  container stop that way).
- The file log `/var/log/aember-init.log` rotates (1 MiB × 3).

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
4. `BuildStep`: `cmake --build --parallel <jobs>`, then merges
   `compile_commands.json` for clangd. **Jobs = 66% of the CPUs**
   (`config/build.py`, override `AEMBER_BUILD_JOBS=<n>`): using every core
   crashes WSL. Configure and build also export it as `AEMBER_BUILD_JOBS`
   and `VCPKG_MAX_CONCURRENCY`, and the toolchain files use it for
   `-flto=<jobs>`.
5. `PackStep` (`steps/packaging/`): initramfs tree + BusyBox (installed with
   `qemu-<arch>-static` when cross) + `aember` + examples/tests + `/init`
   symlink; runtime libs resolved with `readelf -d` (NEEDED; works for every
   arch, unlike the host objdump) from the LXC
   libs and the SDK target sysroot; kernel modules from `modules.json` +
   `depmod`; `aember-initramfs.cpio.gz`; `aember-rootfs.img` built with
   guestfish in a `docker run` of the current dev image (bind mount via
   `HOST_ROOT_PATH`)

Every name in `resources/<arch>/rootfs/etc/aember/modules.json` must exist in
the aember-resources kernel, as a `.ko` or listed in `modules.builtin`
(bridge, veth, br_netfilter, overlay, llc, stp, ttm are built in). The
packer fails on anything missing, packs each listed `.ko` **plus its
dependencies** (from the `modules.dep` the kernel asset ships since
aember-resources v0.1.4; e.g. virtio-gpu needs drm_shmem_helper), and
copies `modules.builtin*`/`modules.order` into the rootfs. BusyBox `modprobe`
does **not** read `modules.builtin`, so the C++ `ModuleLoader` checks
`/lib/modules/<release>/modules.builtin` itself and skips built-in modules.

`aember qemu launch` boots `build/package/<arch>/<BuildType>/` in
`qemu-system-x86_64` / `qemu-system-aarch64 -machine virt` /
`qemu-system-arm -machine virt,highmem=off -cpu cortex-a15`, in xterm windows
(Debug also starts GDB). **`aember qemu launch --headless --timeout 30`**
runs QEMU in the current terminal with the serial console on stdout, for
checks without a display. aember is PID 1 and never exits, so the timeout is
what ends the run (or, from the debug shell, `kill -USR2 1` powers the VM
off and QEMU ends by itself); the boot itself takes ~5 s.

Debug shell: boot never waits for it. Press `d` on the console at any time
(a listener thread in `utils/shell/debug-shell.cpp` watches it; after the
shell exits, `d` opens a new one). `aember.shell=1` on the kernel command
line opens one at boot: `aember app launch --debug-shell` adds it, on a
board it goes into the boot loader's command line (`cmdline.txt` on a
Raspberry Pi). If the switch to the real root fails, the initramfs opens
an emergency shell by itself. While a shell is open PID 1's console log is
muted (`set_console_logging`); the file log keeps everything. In headless
QEMU, type into the terminal (or pipe into `docker exec -i`). Headless keeps the virtual
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

## Releasing aember

1. Bump `project(VERSION)` in `CMakeLists.txt` and write
   `doc/changelog/`. `aember changelog --version-bump minor|patch|major`
   compares `main..HEAD`, so it only finds changes on a feature branch; on
   `main` run its `ChangelogStep` with `base_branch` set to the last tag.
2. `aember release` (every architecture by default, `-a` repeatable to pick
   some): for each architecture and build type it fetches resources,
   configures, builds, packs and writes
   `build/release/<arch>/<type>/`: `aember-app-<arch>-<type>-v<V>.tar.gz`
   (kernel, initramfs, rootfs image), `aember-binary-…`, and
   `aember-resources-v<V>.tar.gz` + `.sha256` (the overlay).
3. Commit and push, then `aember publish [--clobber]`
   (`steps/releasing/publish.py`): it checks the overlay tarballs against
   their `.sha256` and that HEAD is pushed, creates the GitHub release `v<V>`
   at HEAD (notes: that version's `doc/changelog/` file) and uploads the
   overlay tarball **once** plus every app and binary tarball. Existing
   assets are refused unless `--clobber`. A fresh clone needs
   `aember-resources-v<project VERSION>.tar.gz` in release `v<VERSION>`, so
   a version bump must always be followed by its release.

Kernel, BusyBox, LXC and SDK are no longer part of aember's releases; they
live in aember-resources' releases.

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
  (GTest, `<lib>-unittest`, `add_test`). Every library has tests (18
  suites, ~150 tests); only what needs root, real interfaces or LXC
  containers is left to the QEMU boots.
- Tests build only with `--with-tests`, which forces x64 Debug; the test
  binaries are packed into the initramfs (run them in the VM from the debug
  shell) and run in the dev container with **`aember test`**
  (`-k <name>` to filter, `-n <N>` to repeat): it starts each binary through
  the SDK sysroot's loader with the SDK and LXC libs on the library path.
- The rootfs image is sized from its contents (at least `rootfs_size`,
  512 MB); a Debug build with tests needs ~700 MB. A guestfish error fails
  the pack (guestfish itself can exit 0 after a failed `copy-in`).
- PID 1's file log survives in the rootfs image after a run: copy partition
  1 (`dd skip=128`), `e2fsck -fy` it (QEMU was killed, so the ext4 journal
  must be replayed), then `debugfs -R "cat /var/log/aember-init.log"`.

CMake: cmake-format (`.cmake-format.yaml`) and cmakelint (`.cmakelintrc`).

Python (aember-cli): Typer commands in `cli/`, `Step` subclasses in
`steps/<area>/`, `Pipeline([...]).run(ctx)`, per-arch data in `config/`,
`log = logging.getLogger("aember.<area>")` with `%s` args. Run `aember lint`
(ruff + black + clang-format + cmake-format/cmakelint) before finishing.

## Submodules

Only `vcpkg` (fork, with the poky triplets) is a submodule. Don't bump its
pointer without being asked. `aember-cli/` used to be a submodule
(ajdariarian94/aember-cli, now **archived** and read-only; it only holds the
CLI's history up to `af5bc15`). It's regular files here: edit it directly.

## Environment

- Work inside the dev container: the repo is at `/aember_ws/aember`, venv
  `/venv`, privileged, `--net=host`, Docker via the host socket,
  `HOST_ROOT_PATH` = host path of the repo (needed by the guestfish `docker
  run`). `tools/scripts/init.sh` fixes the Docker GID and sets up the shell
  on start.
- `/venv/bin/aember` is a symlink to `aember-cli/run.py`, created at
  container start by `tools/scripts/init.sh` (via `bashrc.d/setup_prompt.sh`).
  A plain `docker run` of the dev image must run `init.sh` first. In a
  non-interactive `docker exec`, put `/venv/bin` on `PATH`; without the
  symlink, `/venv/bin/python aember-cli/run.py <args>` works the same.
- `devcontainer.json` uses the prebuilt image; switch to the `build:` block
  to test Dockerfile changes.
- Downloads (overlay images, aember-resources) use plain HTTPS; `gh` and a
  token are not needed to build. For `aember publish` and other `gh` use,
  `GITHUB_TOKEN` can come from `tools/scripts/.env` (git-ignored, **never
  commit or print it**, `utils/github.load_github_token`) or a
  `gh auth login`. With a login, `init.sh` runs `gh auth setup-git` at
  container start, so plain `git push` works.
- `aember-cli/aember/config/host_path.py` reads `HOST_ROOT_PATH` at import.

## Known issues (don't fix unless asked)

- `cli/qemu/build.py`: `if build_tests or BuildType.debug:` is always true,
  so every `aember qemu build` starts with `CleanStep`.
- The dev Dockerfile still has Yocto-era leftovers (GNU tar 1.34
  downgrade, gcc-12/14 PPA). Harmless, but can go on the next image rebuild.

## Working agreements for agents

- `TODO.md` (repo root) lists collected ideas and known gaps. Read it for
  context, but implement an item only when the user asks for it; update it
  when an item is done or a new one comes up.
- Branch off `main` (`fix/<x>`, `enhancement/<x>`, `docs/<x>`, …) and merge
  through a PR. Commit only when asked.
- Don't commit unrelated working-tree changes the user has in progress.
- Confirm before deleting or overwriting `resources/` or `build/`
  (especially `build/aember-resources/`): they're expensive to regenerate.
- Don't publish GitHub releases or upload assets unless explicitly asked.
- Don't start long builds without the user asking.
- Keep this file current. Update it in the same change when something here
  goes stale.
