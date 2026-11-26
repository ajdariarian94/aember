# AGENTS.md

Guidance for AI coding agents (Claude Code, Copilot, Codex, …) working in
**aember**.

## What this repo is

**Aember** is a PID 1 (init system) for constrained embedded Linux, written in
modern C++ (**C++23**). It supervises processes, runs LXC containers, loads
kernel modules, brings up the network and pivots from the initramfs to the
real root filesystem. This repo holds:

- the C++ code: `libraries/` (the init system as static libs), `executables/`
  (the `aember` binary, PID 1), `services/` (separate programs PID 1
  starts from services.json: `aember-netd`, `aember-containerd`
  and whatever else you add)
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
             mounts, loads modules, mounts the real root, switch_root
          ─► real root: aember --root (PID 1)           AemberInit::StartRoot()
             mounts, modules, reaping, signals, services, heartbeat
             └─ services.json, e.g.
                ├─ /usr/bin/aember-netd        network (services/netd)
                └─ /usr/bin/aember-containerd  LXC containers + bridges (services/containerd)
```

`executables/aember/main.cpp` is PID 1 only: CLI11 picks the mode
(`--root`, `--chroot`). It contains no network or container code.

**Services (`services/`, own programs).** Network and containers are
ordinary services: a board lists them in its `services.json` or doesn't
(e.g. ConnMan instead of `aember-netd`), and then their code isn't in the
image at all. A crash in them never takes PID 1 down.
- Build: `services/` is its own ExternalProject (after `libraries`); every
  subdirectory with a CMakeLists.txt is a service, built against the
  installed aember-libs with `aember_service()`
  (`services/cmake/AemberService.cmake`: `<name>-core` static lib for the
  tests + the executable, stripped in Release, installed to
  `build/install/<arch>/<type>/services/`). How to write one:
  `services/README.md`. Headers: `<aember-services/<name>/...>`,
  namespaces `aember::netd`, `aember::containerd`.
- Pack: only the services the overlay's `services.json` starts
  (`/usr/bin/<name>` + a built `services/<name>`) go into `services_root`,
  a rootfs-only tree (no initramfs RAM), with the libraries they need
  that the initramfs doesn't have (NEEDED, plus `SERVICE_DLOPEN_LIBS` in
  `config/libs.py`: liblxc for aember-containerd).
- services.json entries for them: `"ipc": true`, `"ready_timeout_ms":
  120000`, always restarted (`max_restarts` 0, 2 s), `stop_timeout_ms`
  10 s (netd) / 60 s (containerd); containerd depends on netd in the
  default configs (drop that when the board has no netd).
- Channel: `utils/ipc` (`Channel`: SOCK_SEQPACKET socketpair, the daemon
  gets its end as **fd 3**, one JSON message per packet; `Hub`: PID 1's
  thread polling all channels). Messages: `{"type":"ready"}`,
  `{"type":"event","name":"network"|"container","data":{...}}`. PID 1
  keeps the last network status and container states for the heartbeat.
  PID 1 → daemon: `{"type":"console","enabled":bool}` (debug shell
  open/closed) through `Hub::Publish`, which also sends the last value to
  channels added later, never blocking; ProcessManager hands the channel
  to the hub *before* the fork, so it is waiting when the daemon starts.
  Daemons call `ipc::ReceiveControl` right after `Channel::Inherited()`
  and whenever fd 3 is readable.
- Readiness: a service with `ipc` stays Starting until "ready"
  (`ServiceManager::MarkReady`) or its `ready_timeout`; services whose
  dependencies aren't running wait and start as soon as they are (also on
  restart). `StopAll` stops dependents first (containerd before netd).
- netd: brings up the interfaces (keeps existing addresses), reports
  status on change and every 10 s; a DHCP interface that isn't
  `required` and has no usable link (`netlink::LinkUsable`: up, carrier,
  not dormant) after 3 s waits (`kWaiting`)
  instead of spending its DHCP attempts, and `MaintainLeases` (every
  10 s) brings it up once the link appears (Wi-Fi after it associated, a
  cable plugged in later); a failed one that isn't required is retried
  every 60 s. A required interface keeps its DHCP attempts at boot; on SIGTERM it exits and leaves the
  kernel's network configuration, so a restart doesn't drop the network.
- containerd: creates the container bridges (`"bridges"` in
  containers.json, `netlink::EnsureBridge`; network.json no longer has
  them, so containers work with any network manager), starts the
  containers from containers.json, or **adopts**
  the ones still running after a crash/restart (LXC keeps their state);
  follows each container's init with a pidfd; SIGTERM stops all
  containers.
  - **Init wrapper** (containers.json `"init"`, default true): the app
    runs under `/busybox.aember-init sh -c <trap script>` (the host's
    static `/bin/busybox` bind mounted into the rootfs; the name must
    start with "busybox" or BusyBox looks for an applet of that name).
    The wrapper is the namespace init and passes SIGTERM/INT/HUP on; an
    app that is init itself never gets SIGTERM. `lxc.signal.halt` =
    SIGTERM, graceful stop waits `"stop_timeout"` (10 s). The script is
    `kInitScript` in `init-wrapper.h` (tested with the host's sh in
    `init-wrapper-unittest`). Inside a container (`container=lxc`) with a
    console (`/dev/console` a char device that opens) it first sends the
    app's stdin/stdout/stderr to `/dev/null`/the console: LXC gives a
    daemonized init `/dev/null`, so app output was lost; the console pty
    is what LXC reads, and `lxc.console.logfile` in the container's
    config keeps it (aember-build-helper writes
    `/var/log/aember-container-<name>.log`, 1 MB, rotated).
  - **Control socket** (`control.h`): `/run/aember-containerd/control.sock`
    (directory 0700), SOCK_SEQPACKET, one JSON request per connection:
    `{"op":"list"}`, `{"op":"stop"|"start"|"restart","name":...}`, served
    in containerd's poll loop. A stop through it is a requested stop (a
    pending restart is dropped, `restart_policy` doesn't restart it); a
    failed start schedules a restart like any other. The aember-fleet
    agent (container) binds `/run/aember-containerd` to update container
    images: stop, swap the SquashFS, start (a start mounts the image
    again). Any container given that directory controls all containers.
  - containerd's `pthread_atfork` child handler unblocks all signals:
    liblxc forks without exec and gives the container init the caller's
    mask, so with containerd's signalfd mask nothing in a container got
    SIGTERM and shells there never saw SIGCHLD.
  - **Restarts:** `"restart_policy"` `no` (default) / `on-failure` /
    `always`, `"max_restart_attempts"` (0 = no limit; reset after 60 s of
    running), `"restart_delay"` (s). `ContainerManager::NextRestart`
    decides, containerd's loop schedules; a requested stop never
    restarts. containerd can't see the exit status (not the parent), so
    every unrequested exit is a failure.
  - Stop unmounts innermost first (`busybox.aember-init`, `host-logs`,
    the overlay), then the SquashFS lowerdir (1 s retry, then lazy
    detach). It is deliberately **not a subreaper**: the LXC monitors have
  a parent-death signal, which also fires when a subreaper they were
  reparented to dies; they stay PID 1's orphans.
- LXC forks its monitors from containerd without exec, so they show
  containerd's command line: `{aember-echo} /usr/bin/aember-containerd`.
  Match `{aember-ctrd}` (containerd's own name), not the command line,
  when you look for or kill containerd.

## aember-resources (kernel, BusyBox, LXC, SDK)

aember-resources (`../aember-resources`,
<https://github.com/ajdariarian94/aember-resources>) builds the low-level
pieces with kas/Yocto and publishes them as assets of the GitHub release
`v<version>`. The version aember uses is pinned in
`aember-cli/aember/config/resources.py` (`RESOURCES_VERSION`, now `0.1.14`;
all 8 targets, with the opt-in packages).

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
.devcontainer/          Ubuntu 24.04 dev image (published as ghcr.io/ajdariarian94/aember-dev
                        by .github/workflows/dev-image.yml; pulled, never built)
aember-cli/             the `aember` CLI (Python, Typer): cli/, steps/, config/, utils/
third_party/            vendored sources, unmodified (monocypher: SHA-512, Ed25519); not linted or analyzed
CMakeLists.txt          Superbuild: ExternalProject libraries → executables, services
                        project VERSION here is the app version (CLI reads it)
libraries/              aember-libs: one static lib per dir, + utils/<x>
executables/aember/     main.cpp → the `aember` binary (PID 1)
services/               one dir per service program (netd, containerd), README.md
resources/<arch>/rootfs rootfs overlay: configs tracked, *.squashfs images from the release
tools/toolchains/       <arch>.cmake, chain-loaded by vcpkg (SDK compilers, sysroot, flags)
tools/scripts/          init.sh (postStart), prerequisites.sh, bashrc.d/
vcpkg/                  SUBMODULE (fork) incl. x64/aarch64-poky-linux triplets
tools/godbolt/          Dockerfile: upstream Compiler Explorer at a pinned commit (`aember godbolt`)
tools/triplets/         overlay triplets not in the submodule (arm-poky-linux, riscv64-poky-linux),
                        passed as VCPKG_OVERLAY_TRIPLETS by ConfigureStep
vcpkg.json              cli11, gtest, nlohmann-json, spdlog
spec/tla/               TLA+ models checked with TLC (spec/tla/check.sh, CI job `tla`):
                        StartGraph = start graph, restarts, shutdown; README.md
doc/                    Doxyfile, changelog/ (written by `aember changelog`), configuration.md
                        (every /etc/aember/*.json key: keep it current with the parsers), packages.md,
                        licensing/ (third-party inventory, components.json, licence texts,
                        source offer, notes for the legal review, CLA draft)
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
| `process-manager`   | Spawns and tracks native processes (optionally with an ipc channel on fd 3) |
| `child-supervisor`  | Tracks service pids; `ReapZombies()` is PID 1's only reaper (see below) |
| `service-manager`   | PID 1's service supervisor: dependency order, readiness, stop order |
| `device-health`     | Heartbeat + `StatusReporter`: PID 1's status file `/var/log/aember-status.json` (read by the aember-stats container) |
| `utils/*`           | config, ipc (service channels), logging (spdlog), module, mount, netlink (rtnetlink), package (`.aember`: verify, unpack, install, rollback; doc/aember-format.md), process, root, shell, signal (signalfd + epoll) |

Services (`services/<name>/`, not libraries): `netd` (interfaces, DHCP,
DNS, connectivity; `network.json`), `containerd` (LXC lifecycle, bridges;
`containers.json`; liblxc via `dlopen` in `lxc-api.h`, headers from
`VIRT_ROOT`).

**Logging:** one spdlog logger for the whole process
(`utils/logging`: `global_logger()`, the spdlog default logger). A
`Logger{"component"}` is only a name: it logs `[component] message`
through the global logger, whose single sink is a thread-safe `dist_sink`
(console, plus the rotating file once `enable_file_logging()` ran). Don't
create spdlog loggers or call `std::print*`/`printf` in the libraries.

**No BusyBox in the boot path:** PID 1 runs no external programs except
services, the debug shell (`/bin/sh`) and LXC. Each former BusyBox call is
a syscall now:
- SquashFS: `MountManager::AttachLoop` (`/dev/loop-control`,
  `LOOP_CONFIGURE`, read-only + autoclear) + `mount(2)`. Keep the
  `LoopDevice` handle (its fd) alive until `mount()` returned: with
  autoclear the device detaches when the last fd closes.
- Modules: `ModuleLoader` walks `modules.dep` (dependencies first) and
  calls `finit_module(2)`; built-ins from `modules.builtin`.
- Network: `utils/netlink` (rtnetlink: link up, address, default route,
  bridge, veth, delete) instead of `ip`; DHCP is netd's
  `dhcp-client.{h,cpp}` (DISCOVER/OFFER/REQUEST/ACK on an `AF_PACKET`
  socket with self-built IPv4/UDP headers, source 0.0.0.0, like udhcpc).
  `NetworkManager::MaintainLeases` (netd calls it every 10 s) renews
  after T1 (unicast) and rebinds after T2 (broadcast) over a UDP socket,
  starts over on a NAK or at expiry; a restarted netd that finds the
  address already up confirms it (INIT-REBOOT), and keeps it without a
  lease when no server answers (chroot under Docker). No DHCPRELEASE:
  netd restarts must not drop the network. Root tests use a raw-frame
  fake server on a veth pair in a private net + mount namespace (a temp
  file over /etc/resolv.conf); the stack's UDP checksums are still
  partial (offload) when the peer's AF_PACKET socket sees them.
- Root switch: `RootManager::SwitchRoot` frees the initramfs (only if `/`
  is ramfs/tmpfs; never crosses mounts or follows symlinks), `MS_MOVE`s the
  new root onto `/`, `chroot`s and execs `/usr/bin/aember --root`.
BusyBox stays in the image for the debug shell and interactive use.

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
- The ipc hub catches handler exceptions per message (a throwing handler
  once killed the hub thread, so readiness was never seen again).
- **Hardware watchdog** (`utils/watchdog`, root mode, never with
  `--chroot`): PID 1 opens `/dev/watchdog` after the mount phase (15 s;
  `aember.watchdog=<s>` sets it, `aember.watchdog=0` turns it off). A
  thread feeds it every timeout/3, **but only while the run loop and the
  reaper ticked in the last 10 s** (before the run loop starts, boot gets
  2 min), so a hung PID 1 really resets the board. `Stop()` feeds
  unconditionally while services stop and disarms it (magic close) for a
  requested reboot/poweroff/halt; stopping without a request leaves it
  armed, so the board resets instead of reaping forever. The kernels have
  `i6300esb` (aember-resources v0.1.6) and `aember app launch` adds QEMU's
  `-device i6300esb` (a real countdown, reset on expiry), so QEMU tests
  the watchdog on every architecture; the Pi has `bcm2835_wdt`, x64 PCs
  `iTCO_wdt`/`sp5100_tco` (aember-resources v0.1.7). **QEMU's
  raspi machines reset the instant the BCM2835 watchdog starts** (no
  countdown), so `aember rpi launch` boots with `aember.watchdog=0`.
  Reset check: `aember app launch --headless --append aember.watchdog=0`,
  then `echo 1 > /dev/watchdog` from the debug shell: the VM reboots.
- **Loopback:** PID 1 brings `lo` up in root mode before any service
  starts (`BringUpLoopback`, rtnetlink): it's not the network service's
  job (a board may run ConnMan or nothing), and local clients such as SSH
  to 127.0.0.1 hang while it's down.
- The file logs `/var/log/aember-{init,netd,containerd}.log` rotate
  (1 MiB × 3 each).

Runtime config lives in the overlay at
`resources/<arch>/rootfs/etc/aember/{services,containers,modules,network}.json`;
container images under `rootfs/var/lib/aember/<name>/`.

The status display is two containers from sibling repos, `../aember-stats`
(collector, JSON lines on `/run/aember-stats/stats.sock`) and
`../aember-monitor` (minimal Qt 6 viewer), installed into an overlay with
`aember-cli/run.py install -a <arch> ../aember` there. aember's part is only
PID 1's status file `/var/log/aember-status.json` (`StatusReporter`, read at
`/host-logs/aember-status.json` in containers): keep its format compatible
with aember-stats' (it embeds it unchanged). Each entry of its `services`
list has `name`, `state`, `type` and `since_ms`: when PID 1 saw its last
state change, in ms since the kernel started (CLOCK_BOOTTIME, the clock of
the kernel's console timestamps), so the status is also the boot timeline. The old Qt monitor container,
its per-second console box and the X11/lxc-extra.conf passthrough are gone;
don't bring them back.

The overlay's text files (the JSON configs, `lxc.container.conf`) are
**tracked in git**; edit them here and commit. The container
images (`*.squashfs`) are git-ignored and
come from aember's own release `v<project VERSION>` as
`aember-resources-v<V>.tar.gz` (public, plain HTTPS). `CheckResources`
(`utils/check_resources.py`) downloads it when an image that some
board's `containers.json` lists is missing (`utils/containers.py`), and
extracts only files that don't exist yet, so tracked configs always win.
`aember release` packs the whole overlay into that tarball.

**Opt-in packages** (`utils/packages.py`, `steps/packaging/packages.py`,
user docs in `doc/packages.md`): a board's `etc/aember/packages.json`
lists aember-resources packages (`dropbear`, `e2fsprogs`, `wifi`,
`wifi-ap`). `FetchResources` installs only the listed ones (besides
kernel, BusyBox, LXC, SDK) into `build/aember-resources/<arch>/packages/
<name>/` and fails for a listed one the release doesn't have; any manifest
component other than those four is a package (`config/resources.is_package`).
`PackStep` stages `packages_root` (rootfs only, copies): each package's
`rootfs/`, its `symlinks.txt` links and `dirs.txt` directories, a minimal
`/etc/passwd`/`/etc/group`/`/root` (the overlay wins), then
`copy_runtime_libs` for the packages' executables with the package lib
dirs searched first: a package ships everything but the C library, so
the C library pieces they need (glibc's `libpthread.so.0`) come from the
SDK here. `check_service_executables` fails the pack when `services.json`
starts a program no tree has. The Pi/USB images and the AppImage include
`packages_root`.

**Only listed containers ship.** The overlay may hold more container
directories (`var/lib/aember/<name>/`) than a board runs: `PackStep`
stages `build/package/<arch>/<type>/overlay_root` (hard links,
`steps/packaging/overlay.py`) without the ones `containers.json` doesn't
list, and the rootfs image, `aember rpi image`, `aember usb image` and
the AppImage all use that tree, never `resources/<arch>/rootfs` directly.
A listed container whose image or `lxc.container.conf` is missing fails
the pack. Drop a container from `containers.json` and its image is gone
from the next image.

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

Downloads that aren't release assets (Pi firmware, systemd-boot, the
AppImage's runtime and slirp4netns) are pinned by sha256 and go through
`utils/download.fetch_pinned`; `utils/download.sha256_file` is the one file
hash.

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
   `-flto=<jobs>`. Both (and pack) take the build/install trees and the
   SDK shell line from `steps/cmake/sdk_build.py`.
5. `PackStep` (`steps/packaging/`): initramfs tree + BusyBox (installed with
   `qemu-<arch>-static` when cross) + `aember` + `/init`
   symlink; runtime libs resolved with `readelf -d` (NEEDED; works for every
   arch, unlike the host objdump), from the LXC libs and the SDK target
   sysroot; the services listed in services.json into `services_root`
   (rootfs only) with what they need beyond the initramfs (liblxc for
   containerd, `SERVICE_DLOPEN_LIBS`); kernel modules from `modules.json` +
   `depmod`; the overlay with only the listed containers (`overlay_root`,
   above); tests into `tests_root` (rootfs only);
   `aember-initramfs.cpio.gz`; `aember-rootfs.img` built with
   guestfish in a `docker run` of the current dev image (bind mount via
   `HOST_ROOT_PATH`). Trees go in with `tar-in` from tars whose entries are uid/gid 0
   (`rootfs.tar_in_commands`, also for the Pi and USB images): `copy-in`
   kept the build user's uid 1000, and ownership checks on the device
   (an SSH server's `/root`) failed

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
`qemu-system-arm -machine virt,highmem=off -cpu cortex-a15` /
`qemu-system-riscv64 -machine virt` (OpenSBI, console ttyS0; aarch64 runs on
a Cortex-A53, the SDK's generic ARMv8.0-A tune since aember-resources
v0.1.8), in xterm windows
(Debug also starts GDB). **`aember qemu launch --headless --timeout 30`**
runs QEMU in the current terminal with the serial console on stdout, for
checks without a display. aember is PID 1 and never exits, so the timeout is
what ends the run (or, from the debug shell, `kill -USR2 1` powers the VM
off and QEMU ends by itself); the boot itself takes ~5 s. `--qmp PATH`
adds a QMP socket to a headless run (`screendump` grabs the virtual
screen: aember-website's recorder films aember-monitor through it).

**Without QEMU: `aember app launch --chroot`** (also `aember qemu launch
--chroot`; `--timeout N` for unattended runs) runs the packed system on the
host kernel: aember as PID 1 in a chroot of `aember-rootfs.img`, inside a
privileged helper container of the dev image (`steps/launching/chroot.py`).
- The image partition is mounted read-only with a tmpfs overlay: a run
  never changes the image. /proc, /sys, /dev (so /dev/console is your
  terminal: `d` opens the debug shell), /tmp, /run are prepared first;
  aember skips existing mounts. `aember --root --chroot` loads no kernel
  modules (they'd go into the WSL kernel).
- Network: Docker's eth0 already has an address, and aember keeps any
  existing address instead of running DHCP (general rule, also for `ip=`).
- No display passthrough: containers get no X11 socket (the old Qt
  monitor's xcb passthrough and `/etc/aember/lxc-extra.conf` are gone).
  The aember-monitor container draws offscreen without /dev/fb0; its
  `--screenshot` shows the frame.
- WSL's kernel can't mount zstd SquashFS (`CONFIG_SQUASHFS_ZSTD` off): zstd
  container images are repacked once with xz into `build/chroot-cache/<arch>/`
  and bind-mounted over the originals inside the chroot.
- aarch64/arm32 run through qemu-user: a binfmt_misc handler with the F
  flag is registered for that architecture (`aember-qemu-<arch>`, until WSL
  restarts). aember, services and network work; **LXC containers don't**
  (qemu-user can't create namespaces): use the QEMU boot or the board.
- Ctrl-C/timeout = `docker stop` = SIGTERM to aember = clean shutdown; from
  the debug shell `kill -USR2 1` ends the run (exit 130; 129 for reboot).

**Raspberry Pi 4: `aember rpi image | flash | launch`** (`steps/rpi/`,
`config/rpi.py`). The aarch64 build boots on a Pi 4 from an SD card:
- `aember rpi image` (after `aember app build`/`pack` for
  aarch64-poky-linux) writes `build/rpi/aember-rpi4-v<V>.img`: p1 FAT32
  (type 0x0c, `AEMBERBOOT`) with the pinned Raspberry Pi firmware
  (`start4.elf`, `fixup4.dat`, tag `FIRMWARE_TAG`, sha256-checked,
  cached in `build/downloads/rpi-firmware/`), `config.txt`, `cmdline.txt`,
  `kernel8.img`, the initramfs and the device trees from the kernel asset
  (`kernel/dtb/`, aember-resources v0.1.5+); p2 ext4 `aember-root` with the
  same tree as the QEMU rootfs. Size: a power of two (QEMU's -sd needs it).
- Boot: firmware → kernel8.img + initramfs + bcm2711-rpi-4-b.dtb;
  `root=LABEL=aember-root rootwait`. aember has no udev, so
  `RootManager::FindBlockDevice` reads the ext4 superblocks of
  /sys/class/block devices, and `ResolveDevice` waits up to 30 s for the
  card. Serial console: GPIO 14/15 (mini UART, `ttyS1`), 115200 8N1;
  that is /dev/console, so `d` there opens the debug shell.
- `aember rpi flash --device /dev/sdX`: on WSL first attach the card
  reader with usbipd-win (`usbipd attach --wsl`). Refuses virtual disks
  (always), non-removable or >256 GiB devices (unless `--force`), devices
  smaller than the image and mounted ones; asks before writing (`dd` via
  sudo). Or write the image from Windows with Raspberry Pi Imager
  ("Use custom", path `\\wsl.localhost\<distro>\...\build\rpi\`).
- `aember rpi launch [--timeout N]`: QEMU 8.2 has no raspi4b; this boots
  the image in `raspi3b` with the Pi 3 device tree from the same kernel
  (`-snapshot`, the image stays untouched). It tests the Pi boot path (DT,
  SD driver, root by label, serial), not the GPU firmware step.

**x86-64 PCs: `aember usb image | flash | launch`** (`steps/usb/`,
`config/usb.py`). The x64 build boots on a PC from a USB stick (UEFI):
- `aember usb image` (after `aember app build`/`pack` for x64) writes
  `build/usb/aember-x64-usb-v<V>.img`: GPT, p1 EFI system partition
  (FAT32 `AEMBERBOOT`) with systemd-boot as `EFI/BOOT/BOOTX64.EFI`
  (from Ubuntu 24.04's `systemd-boot-efi` 255.4-1ubuntu8 **release
  pocket** deb, pinned by sha256, cached in
  `build/downloads/systemd-boot/`, extracted with `dpkg-deb`), its
  `loader/` config and the kernel + initramfs under `aember/`; p2 ext4
  `aember-root` with the same tree as the QEMU rootfs. UEFI only, Secure
  Boot off (unsigned kernel).
- Boot: firmware → systemd-boot (3 s menu, `loader/entries/*.conf`):
  "aember" (`console=tty0` last: screen + keyboard, `d` opens a shell),
  "aember (serial console)" (`ttyS0` last), "aember (debug shell at
  boot)". The kernel's EFI stub loads the initramfs (LoadFile2);
  `root=LABEL=aember-root` is found like on the Pi (superblocks, 30 s
  wait for the USB disk).
- The x64 kernel (aember-resources v0.1.7) has the UEFI framebuffer
  (`efifb`: screen console and `/dev/fb0` on any GPU), Realtek/Intel
  NICs, NVMe and the iTCO/SP5100 watchdogs; x86_64_defconfig brings the
  EFI stub, USB storage, xHCI, AHCI and USB HID.
- `aember usb flash --device /dev/sdX`: the Pi's flash code
  (`steps/rpi/flash.py`, `kind`/`max_bytes` parameters: USB sticks up
  to 512 GiB without `--force`).
- `aember usb launch [--headless] [--timeout N]`: QEMU q35 with OVMF
  (`/usr/share/ovmf/OVMF.fd`), the image as `usb-storage` on `qemu-xhci`
  (`bootindex=0`, `-snapshot`), e1000e NIC, i6300esb. Headless, the
  launcher picks the serial-console entry by sending Down + Enter over
  the serial port once systemd-boot's menu shows (`MenuPicker`; OVMF's
  console includes the serial port). Tested: boot from USB, efifb fb0,
  the Qt monitor container on it, DHCP, watchdog. Not yet on a real PC.

**AppImage: `aember appimage`** (`steps/appimage/`, `config/appimage.py`)
builds `build/appimage/aember-x86_64-v<V>.AppImage` from the packed x64
Release system: aember on a Linux desktop, isolated.
- Layout: `AppRun` (launcher, as the user), `libexec/aember-sandbox` (root
  helper), `usr/bin/xterm` + its libraries from the dev image (not glibc),
  `usr/bin/slirp4netns` (static, pinned), `rootfs/` (initramfs tree +
  services + overlay). Runtime: uruntime (pinned), which falls back to
  extract-and-run without FUSE; payload SquashFS xz.
- Flow: double-click → AppRun opens its own xterm window → root via
  pkexec (falls back to sudo in the window when pkexec isn't authorized,
  e.g. no polkit agent) → `aember-sandbox`: `unshare` (mount with private
  propagation, pid, net, cgroup, uts, ipc; `--kill-child`), tmpfs overlay
  over the read-only rootfs (mounted from the AppImage by
  `--appimage-offset`, as root can't read the user's FUSE mount), own
  `/dev` (window pty = /dev/console, devpts, 64 loop nodes), slirp4netns
  NAT with `--disable-host-loopback`, then `chroot … aember --root
  --chroot`. Containers get the display through `xhost
  +si:localuser:root` (revoked at exit).
- aember's power-off ends its PID namespace (status 0), reboot starts it
  again; closing the window kills the namespace's PID 1 and the cgroup
  subtree is removed.
- Tested on a clean Ubuntu 24.04 VM: `tools/tests/appimage-vm-test.sh`
  (QEMU + KVM in the dev container, run as root for /dev/kvm; cloud image
  pinned; Xvfb + xdotool type the password). pkexec there needs a logind
  session; its text agent still fails headless ("No session for cookie"),
  so the test exercises the sudo fallback after it.

Debug shell: boot never waits for it. Press `d` on the console at any time
(a listener thread in `utils/shell/debug-shell.cpp` watches it; after the
shell exits, `d` opens a new one). `aember.shell=1` on the kernel command
line opens one at boot: `aember app launch --debug-shell` adds it, on a
board it goes into the boot loader's command line (`cmdline.txt` on a
Raspberry Pi). If the switch to the real root fails, the initramfs opens
an emergency shell by itself. While a shell is open the console log of PID 1
and of its daemons is muted (`set_console_logging`, passed on to netd and
containerd over ipc); the file logs keep everything
(`/var/log/aember-init.log`, `aember-netd.log`, `aember-containerd.log`). In headless
QEMU, type into the terminal (or pipe into `docker exec -i`). Headless keeps the virtual
GPU (`-display none`, not `-vga none`), so /dev/fb0 exists for the
aember-monitor container (`linuxfb`). Serial logs go to
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

## Third-party licences

aember is sold under its own licence, so what ships next to it must allow
that. `doc/licensing/THIRD_PARTY.md` is the inventory (licence, linking,
obligation, blockers); `doc/licensing/components.json` is its
machine-readable copy. `aember app pack` (`steps/packaging/notices.py`)
writes `/usr/share/licenses/aember/NOTICES.gz` into the rootfs (its own
`notices_root` tree, also used by the Pi/USB images and the AppImage): the
source offer, one entry per component the image really contains (found by
`detect` globs in the staged trees, or the Yocto names in an opt-in
package's packages.txt), the vcpkg ports' upstream `copyright` files, and
each licence text once (`doc/licensing/texts/`, SPDX texts from Poky).
Rules:
- A new third-party component on the device (library, package, container
  content, firmware) goes into `components.json` and THIRD_PARTY.md in the
  same PR; `load_components` refuses unknown keys, missing texts and
  duplicate ids (`tests/test_notices.py`).
- Our programs (aember, its services, the aember-* containers) never link
  GPL code; LGPL only dynamically and replaceable; check a dependency's
  licence before adding it. GPL *programs* next to ours (BusyBox, ConnMan)
  are fine with the source offer.
- Don't edit `LICENSE`: the owner decides it (LICENSING-NOTES.md lists the
  open questions).

## Releasing aember

1. Bump `project(VERSION)` in `CMakeLists.txt` and write
   `doc/changelog/`. `aember changelog --version-bump minor|patch|major`
   compares `main..HEAD` (changes on a feature branch); for a release
   branch cut from `main`, `--since v<last version>` counts from the last
   release tag.
2. `aember release` (every architecture by default, `-a` repeatable to pick
   some): for each architecture and build type it fetches resources,
   configures, builds, packs and writes
   `build/release/<arch>/<type>/`: `aember-app-<arch>-<type>-v<V>.tar.gz`
   (kernel, initramfs, rootfs image), `aember-binary-…`, and
   `aember-resources-v<V>.tar.gz` + `.sha256` (the overlay). For the
   Release builds `ReleaseImages` (`steps/releasing/images.py`) also writes
   the flashable images into `build/release/images/`:
   `aember-rpi4-v<V>.img.xz` (aarch64), `aember-x64-usb-v<V>.img.xz` (x64),
   each with a `.sha256`, plus `aember-rpi-imager.json` (a Raspberry Pi
   Imager repository list: url, sizes and sha256 of the compressed and
   extracted image, `devices: ["pi4-64bit"]`) and its icon
   `aember-icon.png`. `rpi-imager --repo https://github.com/<repo>/releases/latest/download/aember-rpi-imager.json`
   then lists aember (the website's Install page points there).
3. Commit and push, then `aember publish [--clobber]`
   (`steps/releasing/publish.py`): it checks the overlay tarballs against
   their `.sha256` and that HEAD is pushed, creates the GitHub release `v<V>`
   at HEAD (notes: that version's `doc/changelog/` file) and uploads the
   overlay tarball **once** plus every app and binary tarball, and the
   images, their `.sha256` and the Imager list of that version. Existing
   assets are refused unless `--clobber`. A fresh clone needs
   `aember-resources-v<project VERSION>.tar.gz` in release `v<VERSION>`, so
   a version bump must always be followed by its release.

Kernel, BusyBox, LXC and SDK are no longer part of aember's releases; they
live in aember-resources' releases.

## Testing, coverage and analysis

Everything runs in the dev container (and in CI, the same commands in the
same image, `.github/workflows/ci.yml`).

| Command | What it does |
|---|---|
| `aember test [--build] [--root]` | CLI tests + every C++ suite (x64 Debug); `--root` runs them through `sudo -n` so the root-only tests (mounts, loop devices, namespaces, the full root switch) run too |
| `aember test --coverage [--root]` | builds the `Debug-coverage` variant, runs, reports per library/service: `build/coverage/index.html`, `components.json` (gcovr with the SDK's gcov; template instantiations merged per line) |
| `aember test --arch x64-poky-linux-musl` | the unit tests of the musl variant (aember-resources v0.1.9+), run natively through the SDK's `ld-musl`; no `--coverage`/`--sanitize` there |
| `aember test --sanitize asan\|tsan [--root]` | `Debug-asan` (Address + Leak + UndefinedBehavior) or `Debug-tsan` variant; the first report fails the suite (`SANITIZER_ENV`). Needs aember-resources v0.1.8+ (the SDK's gcc-sanitizers). `tools/sanitizers/lsan.supp` (via `LSAN_OPTIONS`) hides only the dynamic loader's own allocations |
| `aember test --valgrind` | every suite under memcheck (errors and definite leaks fail; `tools/valgrind/aember.supp`) |
| `aember analyze [--update-baseline]` | clang-tidy (`.clang-tidy`) and cppcheck over the Debug compile database; findings beyond `tools/analysis/*-baseline.txt` fail |
| `aember mutate -k <unit> [-n N] [--root]` | mutation testing of one unit (a library/service directory with `tests/`, or one of its suites): one operator at a time changed in its `src/*.cpp`, all its suites rebuilt and run against each mutant (`--root`: as root, each in a read-only sandbox); survivors = lines no test checks (`build/mutation/<unit>.json` and `.md`); `--list` names the units with their mutant counts. Weekly in CI for every unit (`.github/workflows/mutation.yml`) |
| `aember fuzz [-k <target>] [--time S] [-j N]` | libFuzzer + ASan + UBSan over the code that reads input (`fuzz/`): each target for S seconds (60), up to 4 at once; failing inputs in `build/fuzz/crashes/<target>/`, logs in `build/fuzz/<target>.log`; `--list` names them |
| `aember systemtest [-k <scenario>] [--timeout-scale F] [--kvm/--no-kvm]` | system tests: each scenario boots the packed image (`aember app pack`, Release by default) in its own QEMU and checks the whole system through the serial console; `--list` names them; consoles in `build/system-tests/<arch>/`; `--scenarios FILE --input k=v` runs another repository's scenarios, `--package-dir` boots another image |
| `aember package keygen <name>` / `create --config --lxc --image [--key]` / `inspect FILE [--keys DIR] [--extract DIR]` | `.aember` packages (doc/aember-format.md): an Ed25519 key pair (.key private, .pub for `etc/aember/keys/`), a package from its three files, a package's footer, signature and contents. Python side: `aember/utils/aember_package.py` + `ed25519.py` (pure Python, RFC 8032; no crypto library in the dev image); device side: `libraries/utils/package` |
| `aember godbolt [--port N] [-f FILE] [--build-type T]` / `--stop` | local Compiler Explorer: upstream's, pinned (`tools/godbolt/Dockerfile`), in Docker on 127.0.0.1; one compiler per target configured under `build/configure/<arch>/<type>/` with that build's flags (see "Compiler Explorer" below) |

| `aember lint [--check]` | ruff, black, clang-format, cmake-format, cmakelint; `--check` changes nothing and fails on anything to fix (CI) |

- **Variants** (`Context.instrument`, `tools/toolchains/instrument.cmake`,
  `AEMBER_INSTRUMENT` passed through the superbuild): each has its own
  `build/{configure,install}/x64-poky-linux/Debug-<variant>/`, so they never
  disturb the normal builds. vcpkg's dependencies stay uninstrumented.
  The flags are CMake's `*_FLAGS_INIT`: after changing instrument.cmake,
  delete that variant's two directories (a cached build keeps old flags).
- **Coverage of forked children**: a child that ends with `_exit()` (or
  execs) never writes its gcov counters. Tests that fork call
  `FlushCoverage()` (`__gcov_dump()` under `AEMBER_COVERAGE`, which only
  coverage builds define; a weak reference would not link it) before
  `_exit()`. The root switch execs away, so root-manager's pivot test passes
  without being counted.
- **Warnings**: `tools/cmake/AemberWarnings.cmake` (included by libraries,
  services, executables; not vcpkg) turns on `-Wall -Wextra
  -Wpedantic -Wshadow ...` as errors (`AEMBER_WERROR`, ON). Keep the code
  warning-free; not used: `-Wnull-dereference` (false positives inside
  libstdc++ at -O2).
- **Static analysis**: clang 18 parses the GCC 15 code with the SDK's
  libstdc++ (`--target`, `-isystem`, `-D__cpp_concepts=202002L` for
  `<expected>`, `-Wno-unknown-warning-option`). A `clang-diagnostic-error`
  means clang couldn't compile a file: fix the invocation, never baseline
  it. `concurrency-mt-unsafe` uses glibc's list; `strerror()` is not
  thread-safe, use `std::system_category().message(err)`. Calls that only
  run with one thread (fork children, atfork handlers) carry a
  `NOLINTNEXTLINE(concurrency-mt-unsafe): <why>`. Fix findings rather than growing the baseline; remove what
  you fixed (`--update-baseline` with the fix).
- **Integration tests that run the real thing in a forked child**:
  `aember-init-unittest` runs `AemberInit::StartRoot()` (chroot mode,
  `InitOptions{config_dir, log_dir, console}` pointed at a sandbox under
  `/var/tmp`, a pty as console) with shell-script services, including one
  speaking the ipc protocol on fd 3, and stops it with each shutdown
  signal; `netd-daemon-unittest` / `containerd-daemon-unittest` run
  `RunNetd()` / `RunContainerd()` with a channel on fd 3
  (`services/tests-common/daemon-runner.h`; as root in private mount + net
  namespaces). `container-lifecycle-unittest` drives ContainerManager
  against a fake liblxc (`SetLxcForTesting` in lxc-api.h: its "init" is a
  forked `sleep`), without root. root-manager's pivot test does the whole initramfs → real
  root switch in a private namespace (a tmpfs "initramfs", an ext4 image on
  a writable loop device found by label, exec of a BusyBox script as
  `/usr/bin/aember`; `AEMBER_TEST_BUSYBOX` comes from `aember test`).
- **System tests** (`aember systemtest`, `aember-cli/aember/steps/testing/system/`):
  `vm.py` is the harness: `vm_command()` boots the packed image headless
  with `-snapshot` (the image never changes) and `-no-reboot` (a reboot
  ends QEMU), `Vm` reads the serial console on a thread (escape sequences
  stripped) and waits for patterns with deadlines (`--timeout-scale`
  stretches them; never fixed sleeps). `vm.open_shell()` presses d until
  the prompt; `vm.run(cmd)` / `vm.sh(cmd)` run a command in the debug shell
  between `@@B""n@@` / `@@E""n:$?@@` markers (the `""` keeps the shell's
  echo of the command line from matching) and give its exit code and
  output; `vm.states()` reads `/var/log/aember-status.json`; the boot
  scenario logs the boot timeline from its `since_ms` (when each entry got
  to running; `build/system-tests/<arch>/boot-timeline.json`);
  `vm.wait_until(check, timeout, what)` polls. While the shell is open
  PID 1 keeps its log off the console, so scenarios that watch PID 1's
  log close the shell first (`(sleep 1; kill -USR2 1) & exit`).
  **Adding a scenario** (`scenarios.py`): a function decorated with
  `@scenario("name", "what it shows")` that takes a `System` and uses
  `with system.boot(append=..., extra_args=[...]) as vm:`;
  `system.services()` / `system.containers()` are the packed
  services.json / containers.json; fail with `raise vm.fail("...")`
  (an AssertionError with the end of the console). The fake-device tests
  in `aember-cli/tests/test_systemtest.py` cover the harness without QEMU.
  **Scenarios of other repositories** (a container's end-to-end test, e.g.
  aember-fleet's `tests/e2e/fleet.py` + `tools/e2e.sh`): `aember systemtest
  --scenarios <file.py> --input name=value ...` imports the file (its
  `@scenario`s register; without `-k` only those run), `system.input(name)`
  reads an input, `--package-dir` boots another image (e.g. a release's,
  unpacked under `build/`; configs then come from the image itself via
  guestfish). `system.image_with_containers(bundles, files)` copies the
  packed image (`<log_dir>/<label>.img`) and puts packed containers
  (aember-build-helper bundles, merged into containers.json like `run.py
  install`) and files in with guestfish; `image_copy_out()` gets a file out;
  `boot(rootfs=..., snapshot=False)` keeps what a run writes, for a second
  boot of the same disk (power loss: leaving the `with` kills QEMU).
- **Regression tests** carry a comment starting `Regression:` saying which
  bug they pin down (`grep -rn "Regression:"` lists them). Every bug fix
  gets one; CLI bugs with no better home go in
  `aember-cli/tests/test_regressions.py`.
- **Property tests** state a rule and check it on many generated cases:
  `test-support/include/aember-test/property.h` (`aember::test::ForAll(runs,
  [](Gen& g) {...})` with gtest assertions; seeded cases, no framework:
  RapidCheck through vcpkg would be built for every target triplet). A
  failing case names its seed: `AEMBER_PROPERTY_SEED=<seed>
  AEMBER_PROPERTY_RUNS=1 <test> --gtest_filter=...` reruns it alone; a big
  `AEMBER_PROPERTY_RUNS` runs more. Test CMakeLists add
  `${AEMBER_TEST_SUPPORT_DIR}` (set in libraries/ and services/) to their
  includes. `aember-test/dependency-graph.h` makes random dependency graphs
  (DAGs, cycles, self-loops, unknown names, duplicates) with the reference
  answers (`Startable`: what StartGraph.tla starts, `DependenciesFirst`,
  `Depth`): the parallel start's tests use it. Today: services.json,
  containers.json, modules.json round trips and wrong types, the
  dependency resolver, the ipc channel, DHCP packets/frames/lease timer,
  restart counting against a model; the CLI's (Hypothesis, skipped when it
  isn't installed) in `aember-cli/tests/test_properties.py`. A new parser or
  format gets a round-trip property. Generator values: `Gen::Pick` returns
  a copy (a reference into a temporary vector dangles); for JSON numbers
  judge the type as read back from text (7 is unsigned once parsed).
- **TLA+ models** (`spec/tla/`, README there): `StartGraph.tla` is the
  design of start (dependency graph, parallel), restarts and shutdown.
  Change the model with the behaviour, run `spec/tla/check.sh` (Docker,
  ~15 s), and keep "Where the code differs" in its README current. A bug
  the model finds gets an `EXPECTED TO FAIL` model plus a regression test.
- **Fuzzing** (`fuzz/`, `aember fuzz`, CI job `fuzz`): one target per
  input aember reads: `fuzz_services_json` (parser + start order),
  `fuzz_containers_json` (containers + `ParseBridges`), `fuzz_modules_json`,
  `fuzz_network_json` (netd's `NetworkManager` constructor; a
  `std::exception` is fine, netd exits on it), `fuzz_ipc` (raw bytes through
  a real channel pair into `Channel::Receive`, PID 1's
  `ApplyDaemonMessage` and `ApplyControl`; `Send` of any string),
  `fuzz_control` (`AnswerControlRequest`), `fuzz_dhcp` (`ParseUdpFrame`,
  `Parse`, `LeaseTimer`), `fuzz_status` (`StatusReporter::WriteFile` always
  leaves one JSON document; `ParseUptimeSeconds`). Targets also check
  properties (`fuzz::Fail`): start order respects dependencies, answers are
  JSON, lease times are ordered. Not part of the main build: clang 18 from
  the dev image builds `fuzz/CMakeLists.txt` against the x64 SDK sysroot
  (`--target=x86_64-poky-linux`, `-D__cpp_concepts=202002L` like `aember
  analyze`) and the x64 Debug build's vcpkg packages (configured first if
  missing), with the sysroot's dynamic linker. Code under test must
  therefore be pure enough to compile without liblxc or root: keep parsing
  apart from doing (e.g. `ParseBridges` vs `EnsureBridges`,
  `AnswerControlRequest` vs `ServeControlConnection`,
  `ApplyDaemonMessage` vs `AemberInit::OnDaemonMessage`). A new target:
  `fuzz/targets/fuzz_<name>.cpp` with `LLVMFuzzerTestOneInput`, its name in
  `AEMBER_FUZZ_TARGETS`, its sources in `aember-fuzzed`, seeds in
  `fuzz/corpus/fuzz_<name>/`. A crash gets a fix, a `Regression:` unit test
  and its input in the seeds (`regression-*`). Reproduce with
  `build/fuzz/build/<target> <file>`.
- **Mutation testing** edits the real sources one mutant at a time (a
  `.mutate-backup` next to the file; a killed run is repaired by the next
  one, or by moving the backup back). Restoring touches the file, or make
  keeps the last mutant's object in the normal build. Tests must never
  write to relative paths: a mutated chdir then drops files in the repo.
  Some survivors are equivalent mutants (same behaviour another way):
  judge each one, don't chase 100%. Each suite runs in its own session: a
  mutant that swaps fork()'s `pid == 0` makes a test kill pid 0, its own
  process group, which once took the whole run (and the restore) with it.
  As root (`--root`) each suite runs in a **read-only sandbox**: its own
  PID and mount namespace where every mount is remounted read-only (bind
  remounts: the real mounts stay writable) except fresh tmpfs on `/tmp` and
  `/var/tmp`, ended by a root `timeout`. A root-manager mutant that no
  longer skipped `.`/`..` in the initramfs delete walked out of the test's
  directory and deleted the CI runner's root filesystem; in the dev
  container it would have reached the host's workspace. If the sandbox
  isn't read-only (a probe write to the repo succeeds) the run stops. The weekly `Mutation`
  workflow runs one job per unit (every mutant, as root) and puts a score
  table and the survivors in its summary; scores are in TODO.md.
- **valgrind**: forked children that `_exit` leave their heap behind, so
  only the test process's own leaks count; a signal sent between fork and
  exec can get lost under valgrind (tests wait for the exec first,
  `WaitForExec`). A failing suite's whole output is in `build/test-logs/`
  (CI uploads it as an artifact).
- **nlohmann::json pitfall**: a one-element braced list becomes that
  element, not an array (`{x}` → `x`). Build arrays with
  `nlohmann::json::array(...)` or from a `std::vector`.
- **Dev image**: developers and CI pull `ghcr.io/ajdariarian94/aember-dev` and never
  build it. `.github/workflows/dev-image.yml` publishes it (tags `latest` +
  `<YYYYMMDD>-<sha7>`) when `.devcontainer/Dockerfile` or what it COPYs
  changes on main; PRs touching them build only. A change to the Dockerfile's
  inputs (e.g. a new Python package in `requirements.txt`) reaches developers
  only after that workflow has run: tell them to `docker pull` and rebuild
  the container.
- **CI** (`.github/workflows/ci.yml`, GitHub's free hosted runners for
  public repos): `tools/ci/free-disk.sh` makes room, `tools/ci/run.sh`
  runs a command in the published dev image with the repo at
  `/aember_ws/aember` (the SDK's baked path), privileged. Jobs: lint + CLI
  tests; tests as root + analysis; coverage (artifact + job summary);
  system tests (x64 Release built, packed with `AEMBER_CI_DOCKER=1` so
  pack's guestfish `docker run` reaches the runner's Docker, then
  `aember systemtest --timeout-scale 3`; KVM when the runner has
  /dev/kvm, else TCG; the consoles are an artifact); valgrind nightly. Job summaries come from `$GITHUB_STEP_SUMMARY`
  (`build/ci-summary.md` inside the container).

- **Compiler Explorer** (`aember godbolt`, `aember-cli/aember/steps/godbolt/`):
  for reading the assembly of PID 1 and the libraries per target before and
  after a change. Upstream's Compiler Explorer, never forked or copied:
  `tools/godbolt/Dockerfile` installs a pinned commit (bump `CE_COMMIT`,
  the node version from that commit's `.node-version`, and `IMAGE` in
  `godbolt.py` together); the image (~3.9 GB) is built on first use (a few minutes; `--rebuild-image`).
  It runs with the repo mounted read-only at `/aember_ws/aember` (the path
  the SDKs and `compile_commands.json` are made for), listening on
  127.0.0.1 only, as the calling user. `build/godbolt/` holds the two files
  it writes on every start: `c++.local.properties` (only the aember
  compilers) and `compiler-explorer.local.properties` (60 s compile
  timeout: aember's headers pull in spdlog/fmt/nlohmann).
  - Compilers: one per target with a configured build (all 8 after
    `aember app configure` for each), ids `aember-x64`, `aember-aarch64`,
    `aember-arm`, `aember-riscv64` and `..._musl`; options = that build's
    flags for `FLAGS_FROM` (process-manager: `--sysroot`, `-march`, the
    build's `-O2 ... -O3` (last wins), `-std=c++23`, defines, warnings,
    `-isystem` vcpkg) plus every `-I` of the database, so any
    `#include <aember-libs/...>` works. Dropped: `-flto` (CE compiles one
    file), `-Werror`, `-g`, `-pipe`, `-o`/`-c`. objdump and c++filt are the
    SDK's of the same target; production mode (`NODE_ENV`), short links
    stored in /tmp of the container.
  - Use: paste a function (or `#include` its header and call it), compare
    targets side by side ("Add new > Compiler"), add `-Os` in the options
    box to see the size build; "Compile to binary" (objdump of the SDK)
    shows the real encoding. `aember godbolt -f file.cpp` prints a link that
    opens with that file. Execution is off (cross compilers).
  - Measure before optimizing: which functions matter comes from the boot
    timeline, `perf` and a size breakdown (task 10's measuring part,
    TODO.md), not from reading assembly.

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
  (GTest, `<lib>-unittest`, `add_test`). Every library and service has
  tests (28 suites: 296 tests as `dev` with 23 root-only ones skipped,
  317 as root, plus 66 CLI tests; 84% of the lines, `aember test --coverage`);
  only what needs root, real interfaces or LXC
  containers is left to the QEMU boots.
- Tests build only with `--with-tests`, which forces x64 Debug; the test
  binaries are packed into the initramfs (run them in the VM from the debug
  shell) and run in the dev container with **`aember test`**
  (`-k <name>` to filter, `-n <N>` to repeat): it starts each binary through
  the SDK sysroot's loader with the SDK and LXC libs on the library path.
  It first runs the CLI's own tests (`aember-cli/tests`, stdlib
  `unittest`, no extra packages; `--no-cli` skips them, `-k cli` runs
  only them): pure logic of the CLI (QEMU command lines, which services
  get packed, Pi boot config, the USB image's boot entries and QEMU
  command, flash checks, the release images and Imager list, the
  changelog). Add one for every CLI change.
- `aember app launch --append "<kernel args>"` adds to the kernel command
  line (e.g. `aember.watchdog=0`, `aember.shell=1`).
- Test binaries go into the rootfs image only (`tests_root`), not the
  initramfs: 20 Debug suites (~300 MB) would not fit the VM's 512 MB RAM.
  Tests that need root (loop devices, veth/bridges, the DHCP exchange,
  mount points) skip in the dev container as `dev`; run them in the VM, or
  in the dev container with `docker exec -u root` (it is privileged). As
  root, the mount-manager/root-manager tests first enter a private mount
  namespace and dhcp-netlink a private network namespace (static init), so
  nothing leaks into the system; keep that for any new test that mounts or
  changes interfaces. (A root run without it once mounted tmpfs over the
  dev container's /run and /tmp, hiding docker.sock and the X11 socket.)
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

- Nothing open here right now (the Dockerfile's Yocto-era leftovers are
  gone, 2026-11-19).

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
