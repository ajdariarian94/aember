# AGENTS.md

Guidance for AI coding agents (Claude Code, Copilot, Codex, …) working in
**aember**.

## What this repo is

**Aember** is a PID 1 (init system) for constrained embedded Linux, written in
modern C++. It supervises processes, runs LXC containers, loads kernel
modules, brings up the network and pivots from the initramfs to the real root
filesystem. This repo holds:

- the C++ code: `libraries/` (the init system as static libs), `executables/`
  (the `aember` binary), `examples/`
- the `aember` CLI (`aember-cli/`, a **git submodule**), which configures,
  builds, packs, launches in QEMU and releases the system
- the dev container, toolchain files, vcpkg setup and the rootfs overlay
  (`resources/`)

The low-level pieces (kernel, BusyBox, LXC, Poky SDK) come from the sibling
repo **aember-resources** (`../aember-resources`), see below.

### Boot flow

```
kernel ─► initramfs /init ─► usr/bin/aember            AemberInit::StartInitramfs()
             mounts, loads modules, mounts the real root, pivot_root
          ─► real root: aember --root                   AemberInit::StartRoot()
             services (ProcessManager), LXC containers (ContainerManager),
             network, heartbeat/monitor
```

`executables/aember/main.cpp` picks the mode with CLI11 (`--root`).

## Relationship to aember-resources (read this first)

aember-resources builds kernel, BusyBox, LXC and the Poky SDK with kas and
publishes them as GitHub release assets
(<https://github.com/ajdariarian94/aember-resources/releases>, v0.1.0, x64
only for now):

| Asset                                        | Content                                         |
|----------------------------------------------|-------------------------------------------------|
| `aember-kernel-<arch>-v<V>.zip`              | `bzImage`/`Image`, `System.map`, `lib/modules/` |
| `aember-busybox-<arch>-v<V>.zip`             | static `busybox`                                |
| `aember-lxc-<arch>-v<V>.zip`                 | `libs/liblxc.so*`, headers, `pkgconfig/lxc.pc`, `.conf` files |
| `aember-sdk-<arch>-v<V>.sh`                  | Poky SDK installer (GCC 15, C++23): `sh <it> -d <dir> -y` |
| `manifest.json`, `SHA256SUMS`                | version, kas hash + sha256 per asset            |

**This repo has not been migrated to those assets yet.** It still carries
the old in-repo setup, which aember-resources replaced:

| Here (legacy, still wired up)                                   | aember-resources (current)                       |
|-----------------------------------------------------------------|--------------------------------------------------|
| `yocto/meta-aember`, `yocto/poky` submodule, `yocto/hashes/`    | kas + `meta-aember` + `hashes/` there            |
| `aember kernel/busybox/lxc/sdk build`: bitbake in the dev container, recipe-file hashes | `aember build <component>` in a pinned kas container, CAS keys |
| tar.gz assets on *aember's* releases (`aember-kernel-…tar.gz`, `aember-modules-…`, `aember-sdk-…tar.gz`, `aember-gcc-reflection-…`) | zip / installer assets on aember-resources' releases |
| C++: **gcc-reflection** (P2996), `-std=c++26 -freflection` (`tools/toolchains/*.cmake`, `config/libs.py`) | Poky SDK GCC 15 only, **C++23**, no gcc-reflection |
| aarch64 tune `cortexa57`                                         | `cortexa72` (Raspberry Pi 4)                     |

The plan is for aember to pull the aember-resources assets and use them to
compile and pack the app. **Don't start that migration, and don't "fix" the
legacy pieces one by one, unless asked.** When you do migrate:

- Only `examples/aember-child-demo/main.cpp` uses reflection (`<meta>`,
  `^^`, `define_aggregate`). The libraries use C++23 features
  (`std::expected`, `std::move_only_function`, `std::format`, ranges) and
  should build with `-std=c++23`.
- The SDK has no `pkg-config`; Poky expects the host's (`pkgconf` is in the
  dev container). `lxc.pc` is relocatable (`${pcfiledir}`).
- Don't edit `../aember-resources` from here; it has its own AGENTS.md.

## Repository layout

```
.devcontainer/          Ubuntu 24.04 dev image (prebuilt ghcr.io/ajdariarian94/aember-dev)
aember-cli/             SUBMODULE (ajdariarian94/aember-cli): the `aember` CLI
CMakeLists.txt          Superbuild: ExternalProject libraries → executables, examples
                        project VERSION here is the app version (CLI reads it)
libraries/              aember-libs: one static lib per dir, + utils/<x>
executables/aember/     main.cpp → the `aember` binary (PID 1)
examples/               demo binaries packed into the initramfs
resources/<arch>/rootfs git-ignored rootfs overlay (configs, container images)
tools/toolchains/       <arch>.cmake, chain-loaded by vcpkg (compilers, sysroot, flags)
tools/scripts/          init.sh (postStart), prerequisites.sh, bashrc.d/
vcpkg/                  SUBMODULE (fork) incl. x64/aarch64-poky-linux triplets
vcpkg.json              cli11, gtest, nlohmann-json, spdlog
yocto/                  legacy: meta-aember, poky submodule, hashes/ (see above)
doc/                    Doxyfile, changelog/ (written by `aember changelog`)
build/                  git-ignored: configure/, install/, package/, downloads/
```

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
| `container-manager` | LXC container lifecycle (liblxc) |
| `child-supervisor`  | Reaps children, dispatches PID exit events |
| `service-manager`   | Coordinator over process/container managers, dependency order |
| `device-health`     | Heartbeat + `AemberMonitor` runtime stats |
| `utils/*`           | config, container, logging (spdlog), module, mount, network, process, root, shell, signal (signalfd + epoll) |

Runtime config lives in the overlay at
`resources/<arch>/rootfs/etc/aember/{services,containers,modules,network}.json`;
container images under `rootfs/var/lib/aember/<name>/`.

## Build pipeline (`aember qemu build`)

`aember-cli/aember/cli/qemu/build.py` runs, in order:

1. `CleanStep`, `CheckResources`: fetch `resources/` (`aember-resources-v<V>.tar.gz`
   from *aember's* release; unrelated to the aember-resources repo despite the
   name)
2. `BuildKernel`, `BuildBusybox`, `BuildLxc`, `BuildSdk` (legacy Yocto steps,
   which download from releases when hashes match)
3. `ConfigureStep`: sources the SDK `environment-setup-*`, unsets CC/CXX,
   runs CMake with the vcpkg toolchain chain-loading `tools/toolchains/<arch>.cmake`
   into `build/configure/<arch>/<BuildType>/`
4. `BuildStep`: `cmake --build`, then merges `compile_commands.json` for clangd
5. `PackStep` (`steps/packaging/`): initramfs tree + BusyBox (installed with
   `qemu-<arch>-static` when cross) + `aember` + examples/tests + `/init`
   symlink; runtime libs resolved with `objdump -p` (NEEDED) from the
   toolchain, LXC and sysroot dirs; kernel modules from `modules.json` +
   `depmod`; `aember-initramfs.cpio.gz`; `aember-rootfs.img` built with guestfish in a
   `docker run` of the current dev image (bind mount via `HOST_ROOT_PATH`)

`aember qemu launch` boots `build/package/<arch>/<BuildType>/` in
`qemu-system-x86_64` / `qemu-system-aarch64 -machine virt`.

Other commands: `aember app configure|build|pack|launch`, `aember lint`,
`aember changelog` (bumps `project(VERSION)` in `CMakeLists.txt` and writes
`doc/changelog/`), `aember release` (all arch × build types → tar.gz in
aember's release dir), `aember generate-docs|view-docs` (Doxygen).

Builds need the SDK (and today gcc-reflection) under
`yocto/build/<arch>/`. Don't start Yocto or full builds without the user
asking: they're long and disk-heavy.

## Coding conventions

C++:
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

`.gitmodules`: `aember-cli` (ajdariarian94/aember-cli), `vcpkg` (fork),
`yocto/poky` (fork, branch `master.cache`).

- **CLI changes are commits in `aember-cli/`'s own repo.** Commit and push
  them there first, then commit the updated submodule pointer here.
- Don't bump the `vcpkg` or `yocto/poky` pointers without being asked.

## Environment

- Work inside the dev container: the repo is at `/aember_ws/aember`, venv
  `/venv`, privileged, `--net=host`, Docker via the host socket,
  `HOST_ROOT_PATH` = host path of the repo (needed by the guestfish `docker
  run`). `tools/scripts/init.sh` fixes the Docker GID on start.
- `devcontainer.json` uses the prebuilt image; switch to the `build:` block
  to test Dockerfile changes.
- `gh` downloads release assets; `GITHUB_TOKEN` can come from
  `tools/scripts/.env` (git-ignored, **never commit or print it**).
- Hard-coded `/aember_ws/aember/...` paths exist in `tools/toolchains/*.cmake`
  and `aember-cli/aember/config/libs.py`.

## Known issues (don't fix unless asked)

- `cli/qemu/build.py`: `if build_tests or BuildType.debug:` is always true,
  so every `aember qemu build` starts with `CleanStep`.
- aarch64 uses `cortexa57` (SDK, sysroot, QEMU CPU) while the target and
  aember-resources use `cortexa72`.

## Working agreements for agents

- Branch off `main` (`fix/<x>`, `enhancement/<x>`, `docs/<x>`, …) and merge
  through a PR. Commit only when asked.
- Don't commit unrelated working-tree changes the user has in progress.
- Confirm before deleting or overwriting `resources/`, `build/`,
  `yocto/build/` or `yocto/hashes/`: they're expensive to regenerate.
- Don't publish GitHub releases or upload assets unless explicitly asked.
- Keep this file current. Update it in the same change when something here
  goes stale (for example, once the aember-resources migration lands).
