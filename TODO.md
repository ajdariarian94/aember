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
- [x] ~~The listener starts after the network phase~~: obsolete since the
  network is a service (2026-11-18): the listener starts right after the
  reaper, before any service.
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
- [x] `NetworkManager::Stop` no longer waits out a ping interval (the
  monitor waits on its stop token, 2026-11-18).
- [x] `ProcessManager::ScheduleRestart` keeps its restart threads; the
  destructor stops and joins them (2026-11-18).
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
  - [x] Heartbeat says "Network │ not reported" when no service reports
    the network (2026-11-18).
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

## x86-64 PCs (USB stick)

- [x] **`aember usb image|flash|launch`** (2026-11-19): UEFI USB stick
  image (GPT, systemd-boot, root by label), QEMU test boot from a USB
  stick with OVMF; aember-resources v0.1.7 adds the UEFI framebuffer,
  Realtek/Intel NICs, NVMe and PC watchdogs. Left:
  - [ ] Boot a real PC (Intel and AMD; a Realtek NIC; the iTCO/SP5100
    watchdog really resets).
  - [ ] Legacy BIOS boot (GRUB or syslinux in an MBR hybrid), if needed.
  - [ ] Secure Boot (shim + signed kernel), if needed.
  - [ ] Install to the internal disk (NVMe/SATA) from the stick.
- [x] **Release images** (2026-11-19): `aember release` writes the Pi and
  USB images (`.img.xz`) and a Raspberry Pi Imager repository list;
  `aember publish` uploads them. Not exercised against a real release yet
  (needs the next `aember release` + `publish`); `rpi-imager --repo` with
  the list untested.
- [x] **Container output** (2026-11-19): the init wrapper sends the app's
  output to the container console (it was LXC's `/dev/null`);
  aember-build-helper keeps it with `lxc.console.logfile`. The overlay's
  own containers (aember-echo, aember-monitor) have no console log file
  in their LXC config yet.

## AppImage

- [x] **`aember appimage`** (2026-11-19): x86-64 AppImage, aember in its
  own namespaces with slirp4netns NAT, own window, pkexec/sudo, FUSE or
  extract fallback; tested on a clean Ubuntu 24.04 VM.
- [ ] pkexec's text agent fails without a desktop session agent ("No
  session for cookie"); the launcher falls back to sudo. Test with a real
  desktop agent (GNOME) once.
- [ ] uruntime leaves an empty `/tmp/.mount_aember*.lock` file.
- [ ] Older hosts: xterm and its libraries come from Ubuntu 24.04
  (glibc 2.39); older distributions would need them built elsewhere.

## Smaller cleanups / known issues

- [x] `aember qemu build` cleans only with `--with-tests` (the condition
  was always true, 2026-11-18).
- [ ] Dev Dockerfile has Yocto-era leftovers (GNU tar 1.34 downgrade,
  gcc-12/14 PPA, gcc cross packages); drop them on the next image rebuild.
- [ ] `aember changelog` compares `main..HEAD`, so it finds nothing on
  `main`; let it take the last tag as base (`--since v0.9.0`).
- [x] `merge_compile_command.py` skips its own output and keeps one entry
  per file (2026-11-18).
- [x] `utils/fetch_artifact.py`: the unused gh download helpers are gone
  (2026-11-18).
