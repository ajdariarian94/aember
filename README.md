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
| `x64-poky-linux` | x86-64 | QEMU, PCs; or without a VM (`--chroot`) |
| `aarch64-poky-linux` | ARMv8 (Cortex-A72) | QEMU `virt`, **Raspberry Pi 4** (SD card image) |
| `arm-poky-linux` | ARMv7-A hard-float NEON | QEMU `virt` (Cortex-A15) |

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

## Commands

| Command | What it does |
|---------|--------------|
| `aember app build` | Fetch resources, configure and build (`--architecture`, `--build-type`) |
| `aember app pack` | Initramfs, root filesystem image, kernel into `build/package/` |
| `aember app launch` | Boot in QEMU (`--headless`, `--timeout`, `--debug-shell`, `--append`, `--chroot`) |
| `aember qemu build --with-tests` | x64 Debug build with the unit tests |
| `aember test` | Run the CLI and C++ unit tests (`-k <name>` to filter, `-n <N>` to repeat) |
| `aember rpi image` / `flash` / `launch` | Raspberry Pi 4 SD card image |
| `aember fetch-resources` | Download kernel, BusyBox, LXC and SDK only |
| `aember lint` | ruff, black, clang-format, cmake-format, cmakelint |
| `aember generate-docs` / `view-docs` | Doxygen documentation |
| `aember changelog` / `release` / `publish` | Version bump, release archives, GitHub release |

`aember --help` and `aember <command> --help` list every option.

## Configuration

The root filesystem overlay is `resources/<arch>/rootfs/`; aember reads:

| File | Contents |
|------|----------|
| `/etc/aember/services.json` | Services: executable, arguments, dependencies, restart policy, `ipc` |
| `/etc/aember/containers.json` | Containers (SquashFS image, LXC config, `restart_policy`, `stop_timeout`) and their bridges |
| `/etc/aember/network.json` | Interfaces (DHCP or static), DNS, connectivity check (aember-netd) |
| `/etc/aember/modules.json` | Kernel modules to load |

Logs: `/var/log/aember-init.log`, `aember-netd.log`, `aember-containerd.log`
(rotating), and the heartbeat in `/var/log/aember-monitor.json`.

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
services/      aember-netd, aember-containerd, your services
examples/      demo programs
aember-cli/    the aember command (Python)
resources/     root filesystem overlays per architecture
tools/         toolchain files, dev container scripts
```

[AGENTS.md](AGENTS.md) has the details for contributors (and coding
agents); [TODO.md](TODO.md) the open ideas.
