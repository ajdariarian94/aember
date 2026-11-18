# TODO — aember

Ideas and findings collected so far. **Nothing here is approved yet:** implement
an item only when the user says so. Related items for the low-level pieces
(kernel, BusyBox, LXC, SDK) are in `../aember-resources/TODO.md`.

**Agreed order (2026-11-15):** 1. aarch64 working end to end, 2. arm32,
3. then size optimization (kernel trimming etc.).

## Boot time and the debug shell

Done (2026-11-16): no 5 s wait at boot any more. A console listener opens
a shell when `d` is pressed at any time; `aember.shell=1` on the kernel
command line (`aember app launch --debug-shell`) opens one at boot; the
initramfs opens an emergency shell when the switch to the real root fails.
Left:

- [ ] `aember.shell=initramfs`: stop before the pivot, for root mount
  problems (today the shell only opens there when the pivot fails).
- [ ] The listener starts after the network phase (it needs the reaper),
  so `d` works only after up to 30 s when there is no network.
- [ ] The emergency shell path was tested in the unit tests only, not by
  booting QEMU with a broken `root=`.
- [ ] `d` works in Release images too, as before: anyone at the serial
  console gets a root shell. Decide whether production images should
  disable it (build option or kernel parameter).

## Running without QEMU

Done (2026-11-17): `aember app launch --chroot` runs the packed system as
PID 1 in a chroot on the host kernel (privileged helper container), with
the Qt monitor on the WSLg desktop through X11. Left:

- [ ] aarch64/arm32 containers can't run in the chroot (qemu-user can't
  clone namespaces); only aember, services and network do.
- [ ] The X11 window has no input checks yet (keyboard/mouse into the Qt
  app untested).

## PID 1 robustness

Done (2026-11-16): PID 1 never returns or dies on an exception
(`pid1::Finish`, terminate handler, guarded phases and threads), shutdown
signals (reboot/poweroff/halt), one reaper that doesn't steal children
other code waits for (this was the `RemoveChild: unknown pid` warning),
signals blocked before any thread starts, the ServiceManager stop deadlock,
`ProcessManager::Stop` racing the reaper, a rotating file log, and
re-enabled tests. 2026-11-17: tests for every library (154, `aember test`),
which found and fixed a ServiceManager start deadlock (services with
dependencies), StartAll starting nothing after one bad dependency,
`AemberMonitor::Log` reading missing JSON keys (UB), `ModuleLoader::IsLoaded`
matching any word of /proc/modules, and Debug rootfs images silently cut
off by guestfish. Left:

- [x] **Graceful container stop** (2026-11-18): BusyBox init wrapper +
  `lxc.signal.halt` SIGTERM, signals unblocked in containerd's fork
  children, `stop_timeout`.
- [x] **Container restart policy** (2026-11-18): `restart_policy`,
  `max_restart_attempts`, `restart_delay` are read and applied.
- [ ] At shutdown the SquashFS lower dir is still `EBUSY` after the
  overlay and the bind mounts in it are unmounted for real (now: 1 s
  retry, then lazy detach). Something keeps the overlay superblock
  alive; find out what (LXC monitor fds? a mount namespace copy?).
- [ ] A static `aember-cinit` (tini-like) instead of the BusyBox sh
  wrapper, once the SDK has static glibc (`glibc-staticdev` in
  aember-resources' SDK).
- [ ] `NetworkManager::Stop` waits up to one ping interval (the monitor
  thread sleeps); use a condition variable with the stop token.
- [ ] `ProcessManager::ScheduleRestart` detaches a thread that captures
  `this`; a restart pending at shutdown can touch a destroyed manager.
- [x] **Hardware watchdog** (2026-11-18): `utils/watchdog`, fed only
  while PID 1's run loop and reaper are alive, disarmed for a requested
  shutdown, `aember.watchdog=<s>|0`. Left:
  - [ ] Verify on the real Pi 4 (QEMU raspi resets at once): normal run
    stays up; a hung PID 1 resets the board.
  - [x] QEMU coverage (2026-11-18): `i6300esb` in every kernel
    (aember-resources v0.1.6), `-device i6300esb` in `aember app launch`;
    fed by PID 1 the VM stays up, unfed it resets.

## Architecture

- [x] **Network and containers out of PID 1** (2026-11-18): `aember
  netd` / `aember containerd` (multi-call), `utils/ipc` channels on fd 3
  with readiness, liblxc only `dlopen`ed by containerd, containerd adopts
  running containers after a restart. PID 1 RSS 6.1 → 5.1 MB.
- [x] **Services as their own programs** (2026-11-18): `services/netd`
  → `aember-netd`, `services/containerd` → `aember-containerd`, built by a
  `services` ExternalProject (every subdir is a service, `aember_service()`
  helper, `services/README.md` for community services). PID 1 no longer
  contains them; boards start them from services.json, and only listed
  services are packed (rootfs only). Container bridges moved from netd
  (network.json) to containerd (containers.json); rtnetlink is the public
  `utils/netlink`. Follow-ups:
  - [ ] Board profiles (`resources/boards/<board>/rootfs` over the arch
    overlay), so QEMU and the Pi 4 can pick different services.
  - [ ] ConnMan as an alternative network service (aember-resources:
    connman + dbus + wpa_supplicant recipes, Pi Wi-Fi in the kernel).
  - [ ] Heartbeat: say "network: external" instead of nothing when no
    service reports network status.
  - [ ] Measure shared vs static spdlog/fmt across aember and the services
    (nlohmann is header-only); keep PID 1 static unless it pays off.
- [ ] Merge ProcessManager into ServiceManager: with containers gone they
  keep the same per-name state twice and mirror it through callbacks.
- [ ] Container restart policy: containers.json has `restart_policy`,
  `max_restart_attempts`, `restart_delay`, but nothing reads them (a
  container whose init exits stays failed, as before the split).
- [x] **Daemons quiet while a debug shell is open** (2026-11-18): PID 1
  publishes `{"type":"console","enabled":false|true}` to its daemons
  (`Hub::Publish`, also to ones (re)started meanwhile; the channel goes
  to the hub before the fork, so a new daemon is muted before its first
  line). Left: output that bypasses spdlog (liblxc's own stderr, services
  from services.json, container consoles) still reaches the console.
- [ ] posix_spawn + pidfd for services (fork in a multi-threaded PID 1).
- [x] **No BusyBox calls from PID 1** (2026-11-17): loop + `mount(2)`,
  `finit_module(2)` with `modules.dep`, rtnetlink, a built-in DHCP client
  (AF_PACKET), in-process switch_root.
- [x] **DHCP lease renewal** (2026-11-18): T1/T2 (options 58/59 or
  50%/87.5%), renew unicast / rebind broadcast over UDP, NAK or expiry
  starts over, a restarted netd confirms its address (INIT-REBOOT).
  No DHCPRELEASE on purpose: netd can't tell its own restart (network
  must stay) from a shutdown.
- [ ] Renewing (unicast) is only tested against QEMU's server in a 24 h
  lease, i.e. not in practice; the root tests cover rebinding (the fake
  server has no IP stack to answer ARP).
- [ ] **Simplify the CMake superbuild.** ExternalProject per subtree plus
  install/export/`Config.cmake` for every internal library is heavy and caused
  the stale-cache problem during the migration (a full configure-dir delete
  was needed). One project with `add_subdirectory` is faster and simpler,
  unless the libraries are consumed outside this repo.

## Production readiness (from the "compete with systemd?" discussion)

Positioning: not a systemd replacement, but "PID 1 for container appliances,
smaller and simpler than systemd + Docker". Closest comparison: Pantavisor
(embedded container init on LXC); also balenaOS/Torizon (on systemd) and
BusyBox init / s6 / runit / dinit.

- [ ] **OTA updates with rollback** (A/B rootfs). Most important for fleets.
- [ ] Signed images; seccomp/AppArmor profiles for containers; dropping
  privileges where possible.
- [ ] **Raspberry Pi 4 on real hardware**: `aember rpi image|flash|launch`
  (2026-11-18) build and write an SD image; tested in QEMU raspi3b only
  (no raspi4b in QEMU 8.2). Not yet on the board. Follow-ups: grow the root
  partition to the card size on first boot; HDMI console (vc4 DRM, not in
  the kernel yet); Wi-Fi/Bluetooth (trimmed); config.txt overlays (the
  firmware's .dtbo files are for the downstream device tree).
- [ ] **arm32 (ARMv7-A hf NEON, `arm-poky-linux`)** on real boards (i.MX6,
  STM32MP1, Raspberry Pi 2). Builds, packs and boots in QEMU (`virt`,
  Cortex-A15) since 0.11.0 with aember-resources v0.1.3.
- [ ] aarch64's `alpine` container image is an old aember initramfs copy
  (40 MB: glibc, liblxc, kernel modules). arm32's is just static BusyBox +
  `echo-alpine.sh` (1 MB); rebuild aarch64's the same way.
- [ ] Docs and a small example appliance, so others can try it in an
  afternoon.

## Smaller cleanups / known issues

- [ ] `cli/qemu/build.py`: `if build_tests or BuildType.debug:` is always
  true, so every `aember qemu build` starts with `CleanStep`.
- [ ] Dev Dockerfile has Yocto-era leftovers (GNU tar 1.34 downgrade,
  gcc-12/14 PPA, gcc cross packages); drop them on the next image rebuild.
- [ ] `aember changelog` compares `main..HEAD`, so it finds nothing on
  `main`; let it take the last tag as base (`--since v0.9.0`).
- [ ] `utils/merge_compile_command.py` also merges its own previous output
  (top-level `build/configure/<arch>/<type>/compile_commands.json`), so the
  file gains a copy of every entry on each build, stale flags included.
- [ ] `utils/fetch_artifact.py` still has the old gh-based download helpers;
  remove what's no longer used.
