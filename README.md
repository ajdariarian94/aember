# aember

**aember is a small, robust PID 1 for container appliances.** It boots an
embedded Linux system from the initramfs to the real root filesystem,
supervises your programs and LXC containers, and keeps the device alive:
in C++23, without systemd, without a shell script in the boot path.

Think of it as "PID 1 + a container runtime for one device", smaller and
simpler than systemd + Docker. PID 1 itself only does what an init must
(mounts, kernel modules, reaping, signals, supervising services); the
network and the containers run as separate services that you can swap out
or leave out.

## How it fits together

```
kernel ─► initramfs /init = aember        mounts, loads modules, finds the root
                                          filesystem (also by LABEL=/UUID=),
                                          switch_root
        ─► aember --root (PID 1)          mounts, modules, reaping, signals,
                                          hardware watchdog, heartbeat
             └─ services from /etc/aember/services.json
                  ├─ aember-netd          network: interfaces, DHCP (with lease
                  │                       renewal), DNS, connectivity status
                  ├─ aember-containerd    LXC containers from containers.json,
                  │                       their bridge, restart policies
                  └─ your own programs    started, restarted, stopped in
                                          dependency order
```

- **PID 1 never dies.** Every init phase is guarded; an exception or a
  crashing service never takes the system down. Shutdown works like
  BusyBox init (`reboot`, `poweroff`, `halt`).
- **Services are separate programs.** A board lists the ones it wants in
  `services.json`; only those are packed into the image. Run ConnMan
  instead of `aember-netd`, or no network at all, and none of netd's code
  ships. You can [write your own](services/README.md) in the `services/`
  folder.
- **Containers** run from read-only SquashFS images with an OverlayFS on
  top, stop gracefully (SIGTERM, then a timeout) and restart according to
  `restart_policy` (`no`, `on-failure`, `always`).
- **Hardware watchdog:** PID 1 feeds `/dev/watchdog` only while its main
  loop and its reaper are alive, so a hung init resets the board.
  `aember.watchdog=<seconds>` on the kernel command line sets the timeout,
  `aember.watchdog=0` turns it off. QEMU gets a watchdog device too, so you
  can watch it work there.
- **Debug shell:** press `d` on the console at any time for a root shell
  (the console log goes quiet meanwhile, the log files keep everything);
  `aember.shell=1` opens one at boot.
- **No BusyBox in the boot path:** loop devices, `mount(2)`,
  `finit_module(2)`, rtnetlink and a built-in DHCP client instead of
  `mount`, `modprobe`, `ip` and `udhcpc`.

## Targets

| Target | Architecture | Runs on |
|--------|--------------|---------|
| `x64-poky-linux` | x86-64 | QEMU, **PCs** (USB stick image, UEFI); or without a VM (`--chroot`, AppImage) |
| `aarch64-poky-linux` | ARMv8.0-A (generic, Cortex-A53 tune) | QEMU `virt`, **Raspberry Pi 4** (SD card image), any 64-bit Arm board |
| `arm-poky-linux` | ARMv7-A hard-float NEON | QEMU `virt` (Cortex-A15) |
| `riscv64-poky-linux` | RISC-V RV64GC | QEMU `virt` (OpenSBI) |
| `x64-poky-linux-musl` | x86-64, musl libc (opt-in) | as x64: ~35% smaller initramfs; no sanitizers |
| `aarch64-poky-linux-musl` | ARMv8.0-A, musl libc (opt-in) | as aarch64 (QEMU `virt`): ~28% smaller initramfs |
| `arm-poky-linux-musl` | ARMv7-A hf NEON, musl libc (opt-in) | as arm32 (QEMU `virt`): ~24% smaller initramfs |
| `riscv64-poky-linux-musl` | RISC-V RV64GC, musl libc (opt-in) | as riscv64 (QEMU `virt`): ~28% smaller initramfs |

The kernel, BusyBox, LXC and the cross toolchains (Poky SDK, GCC 15) come
prebuilt from [aember-resources](https://github.com/ajdariarian94/aember-resources);
the build downloads them.

## Quick start

You need Linux (or WSL2) with Docker and VS Code with the Dev Containers
extension. `./tools/scripts/prerequisites.sh` checks and sets up the host.
Open the repository in the dev container; every command below runs there.

```sh
aember app build                     # first run: downloads everything (~5 min)
aember app pack                      # initramfs + root filesystem image
aember app launch --headless --timeout 60   # boot it in QEMU, console here
```

`aember app launch` without `--headless` opens QEMU and its console in
windows. `--append "<args>"` adds to the kernel command line, e.g.
`--append aember.shell=1` for a shell at boot (`--debug-shell` does the
same). Without a VM at all, aember runs as PID 1 in a chroot on your
kernel:

```sh
aember app launch --chroot --timeout 60
```

Other architectures: add `--architecture aarch64-poky-linux` (or
`arm-poky-linux`) to `build`, `pack` and `launch`.

### Raspberry Pi 4

```sh
aember app build --architecture aarch64-poky-linux
aember app pack --architecture aarch64-poky-linux
aember rpi image                     # build/rpi/aember-rpi4-v<version>.img
aember rpi flash --device /dev/sdX   # or write it with Raspberry Pi Imager
aember rpi launch --timeout 60       # smoke test in QEMU (raspi3b)
```

The serial console is on GPIO 14/15 (115200 8N1); press `d` there for a
shell. On WSL, attach the card reader first with `usbipd attach --wsl`.

### A PC from a USB stick

```sh
aember app build && aember app pack
aember usb image                     # build/usb/aember-x64-usb-v<version>.img
aember usb flash --device /dev/sdX   # or balenaEtcher / Raspberry Pi Imager "Use custom"
aember usb launch --headless --timeout 90   # boot it in QEMU: UEFI, USB stick
```

Boot the PC from the stick in **UEFI mode with Secure Boot off** (the
kernel is not signed; there is no legacy BIOS boot). systemd-boot offers
aember (console on the screen; `d` on the keyboard opens a shell), aember
with the console on the serial port, and aember with a shell at boot.
Nothing on the PC's own disks is touched.

### Ready-made images

`aember release` also writes the flashable images of a version into
`build/release/images/` and `aember publish` uploads them:
`aember-rpi4-v<V>.img.xz`, `aember-x64-usb-v<V>.img.xz` (+ `.sha256`) and
`aember-rpi-imager.json`, a Raspberry Pi Imager repository list. With it,
Imager lists aember: insert the card, pick aember, write:

```sh
rpi-imager --repo https://github.com/ajdariarian94/aember/releases/latest/download/aember-rpi-imager.json
```

balenaEtcher's "Flash from URL" takes the `.img.xz` links directly.

### Try it on your Linux desktop

```sh
aember appimage                      # build/appimage/aember-x86_64-v<version>.AppImage
```

Double-click the AppImage: it opens a window, asks for your password and
runs aember there as PID 1 in its own process, mount and network
namespaces (with a private, NATed network). It never touches your
machine's network; closing the window ends it and cleans up.

## Commands

| Command | What it does |
|---------|--------------|
| `aember app build` | Fetch resources, configure and build (`--architecture`, `--build-type`) |
| `aember app pack` | Initramfs, root filesystem image, kernel into `build/package/` |
| `aember app launch` | Boot in QEMU (`--headless`, `--timeout`, `--debug-shell`, `--append`, `--chroot`) |
| `aember qemu build --with-tests` | x64 Debug build with the unit tests |
| `aember test` | Run the CLI and C++ unit tests (`-k <name>` to filter, `-n <N>` to repeat, `--root` for the root-only tests) |
| `aember test --coverage` / `--sanitize asan\|tsan` / `--valgrind` | Coverage report per library, sanitizer builds, memcheck |
| `aember analyze` | Static analysis (clang-tidy, cppcheck) against a baseline |
| `aember mutate -k <suite>` | Mutation testing of one library's tests |
| `aember rpi image` / `flash` / `launch` | Raspberry Pi 4 SD card image |
| `aember usb image` / `flash` / `launch` | x86-64 PC USB stick image (UEFI) |
| `aember fetch-resources` | Download kernel, BusyBox, LXC and SDK only |
| `aember lint [--check]` | ruff, black, clang-format, cmake-format, cmakelint |
| `aember generate-docs` / `view-docs` | Doxygen documentation |
| `aember appimage` | x86-64 AppImage: aember isolated on a Linux desktop |
| `aember changelog` / `release` / `publish` | Version bump, release archives, GitHub release |

`aember --help` and `aember <command> --help` list every option.

Every push and pull request runs the same commands on GitHub Actions
(`.github/workflows/ci.yml`): lint, all tests as root, static analysis and
the coverage report; valgrind runs nightly. AGENTS.md ("Testing, coverage
and analysis") has the details.

## Configuration

The root filesystem overlay is `resources/<arch>/rootfs/`; aember reads:

| File | Contents |
|------|----------|
| `/etc/aember/services.json` | Services: executable, arguments, dependencies, restart policy, `ipc` |
| `/etc/aember/containers.json` | Containers (SquashFS image, LXC config, `restart_policy`, `stop_timeout`) and their bridges |
| `/etc/aember/network.json` | Interfaces (DHCP or static), DNS, connectivity check (aember-netd) |
| `/etc/aember/modules.json` | Kernel modules to load |

Logs: `/var/log/aember-init.log`, `aember-netd.log`, `aember-containerd.log`
(rotating), and PID 1's status in `/var/log/aember-status.json`. A
container's output goes to its console; with `lxc.console.logfile` in its
LXC config it is kept, e.g. `/var/log/aember-container-<name>.log`.

## Your application as a container

[aember-build-helper](https://github.com/ajdariarian94/aember-build-helper)
(the sibling repository `../aember-build-helper`)
packs a project into an aember-ready container: add it as a submodule,
describe the container in `aember-container.json` (from a Dockerfile, a
Docker image, or a CMake install with the libraries from the aember SDK),
then its `aember` CLI (`aember-cli/run.py build`, then
`aember-cli/run.py install <this repo>`) puts the SquashFS image, its LXC
config and the `containers.json` entry into the board's overlay.

Only what a board lists goes into its image: the services in
`services.json`, the containers in `containers.json` and the opt-in
packages in `packages.json` (SSH, ext4 tools, Wi-Fi client and access
point; see [doc/packages.md](doc/packages.md)). Remove an entry
and the next `aember app pack` leaves that program or container image out.

## Status display: aember-stats and aember-monitor

Two containers from their own repositories show what a device is doing:
[aember-stats](https://github.com/ajdariarian94/aember-stats) collects CPU,
memory, network, disk, temperatures and PID 1's status
(`/var/log/aember-status.json`, all services and containers) and serves them
as JSON lines on `/run/aember-stats/stats.sock`;
[aember-monitor](https://github.com/ajdariarian94/aember-monitor) draws them
full screen with a minimal Qt 6 (on `/dev/fb0`). The socket directory is the
only link between them (an LXC pre-start hook creates it on the device).
Both are built with the aember-resources SDKs for every target and installed
into an overlay with `aember-cli/run.py install -a <arch> <this repo>` in
their repos. The x64 overlay has them (2.1 MB + 9.5 MB, instead of the old
162 MB Qt monitor).

## Writing a service

A service is any program listed in `services.json`. To build one with the
rest of aember, add a folder under `services/` with a `CMakeLists.txt`:

```cmake
find_package(aember-libs REQUIRED COMPONENTS utils)

aember_service(
  aember-hello
  MAIN src/main.cpp
  LIBRARIES aember-libs::utils::ipc aember-libs::utils::logging
)
```

With `"ipc": true` in its `services.json` entry it gets a channel to
PID 1 on fd 3: it reports when it is ready, sends events for the
heartbeat, and is told when a debug shell owns the console. See
[services/README.md](services/README.md).

## Repository

```
libraries/     PID 1 as static libraries (aember-libs)
executables/   the aember binary
services/      aember-netd, aember-containerd, a demo service, your own
aember-cli/    the aember command (Python)
resources/     root filesystem overlays per architecture
tools/         toolchain files, dev container scripts
```

[AGENTS.md](AGENTS.md) has the details for contributors (and coding
agents); [TODO.md](TODO.md) the open ideas.
