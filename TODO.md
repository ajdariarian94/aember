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

PID 1 performance (task 10; after the parallel start graph, which changes
the boot timeline):

- [x] `aember godbolt` (2026-11-25): a local Compiler Explorer with the
  SDK's cross compilers and aember's flags, to read PID 1's assembly.
- [x] Measure first (2026-11-26, doc/performance.md): boot timeline, CPU
  time (schedstat), wakeups, RSS/PSS (`tools/perf/measure.py`) and a size
  breakdown (`tools/perf/size.py`, bloaty). Done from it: Release `-Os`
  (same boot and idle as `-O3`, 18-24 % smaller), CLI11 out of PID 1, fmt
  instead of `std::format` outside task 9's files: the three programs went
  from 2.7 MB to 1.3 MB (x64).
- [ ] After task 9 (doc/performance.md, "Left for after task 9"): the rest
  of `std::format` (aember-init, status, container-manager); liveness
  ticks at the watchdog interval instead of every second; the status file
  off the root file system (flash wear) or written on change; containerd's
  1 s poll; `lazy_itable_init=0` at pack; `quiet` on the boards' kernel
  command line.
- [ ] The same measurements on the Raspberry Pi 4 (with `perf`).

## Running without QEMU

Done (2026-11-17): `aember app launch --chroot` runs the packed system as
PID 1 in a chroot on the host kernel (privileged helper container). The
X11 passthrough for the old Qt monitor was removed (2026-11-20). Left:

- [ ] aarch64/arm32 containers can't run in the chroot (qemu-user can't
  clone namespaces); only aember, services and network do.

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
  own aember-echo container has no console log file in its LXC config yet.

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

## Image size

Measured 2026-11-20 (x64 Release): initramfs 10 MB, services 6 MB; glibc
(libc, libm, loader) 3.2 MB, libstdc++ 2.5 MB, liblxc 4.8 MB. Biggest wins
first:

- [ ] aarch64's `alpine` test container: 40 MB -> ~1 MB (static BusyBox +
  `echo-alpine.sh`, like arm32 and riscv64; see Production readiness).
- [x] Release images strip the libraries they pack (2026-11-20): liblxc
  from aember-resources has its debug info (for Debug builds and gdb):
  4.8 MB -> 1.1 MB on x64. It is already built without seccomp, apparmor,
  selinux and openssl.
- [ ] Measure static libstdc++ (`-static-libstdc++` with LTO and
  `--gc-sections`) against the shared 2.5 MB for aember's few binaries.
- [ ] Kernel modules and firmware: drop what an image ships but never
  loads (per-target `modules.json` audit).
- [x] **musl as an opt-in extra, not a replacement** (2026-11-20):
  `x64-poky-linux-musl` (aember-resources v0.1.9) next to the glibc
  targets. initramfs 4.32 -> 2.80 MB compressed (-35%: one libc.so instead
  of libc + libm + loader, and a 1.2 MB static BusyBox). glibc stays the
  default with the full tooling (no ASan/TSan on musl). Threads get 1 MB
  stacks (`-Wl,-z,stack-size`, musl's default is 128 KB). Unit tests run
  with `aember test --arch x64-poky-linux-musl`.
- [x] musl for every target (aember-resources v0.1.9-v0.1.12): x64,
  aarch64, arm32, riscv64; initramfs 24-35% smaller.
- [ ] A CI job for the musl unit tests; the Pi SD image is glibc-only.

## Status display (aember-stats + aember-monitor)

- [x] Split from aember (2026-11-20): aember-stats (collector) and
  aember-monitor (slim Qt 6 viewer) in their own repositories, talking over
  `/run/aember-stats/stats.sock`; PID 1 writes `/var/log/aember-status.json`;
  the old 162 MB Qt monitor, its console box and the X11 passthrough are
  gone. x64 overlay: both containers, checked in QEMU (connect + samples).
- [ ] Their images for the other seven targets once the Qt SDKs
  (aember-resources v0.1.13) are released, and in aember's release tarball.
- [ ] Monitor input (touch/keys) for pages: needs libinput/evdev in the
  SDK's Qt (`PACKAGECONFIG:pn-qtbase`).

## Smaller cleanups / known issues

- [x] `aember qemu build` cleans only with `--with-tests` (the condition
  was always true, 2026-11-18).
- [x] Dev Dockerfile: the Yocto-era leftovers (GNU tar 1.34 downgrade,
  gcc-12/14 PPA, gcc cross packages) are gone (2026-11-19).
- [x] `aember changelog --since v<last version>` counts from the last
  release tag instead of `main` (2026-11-19).
- [x] `merge_compile_command.py` skips its own output and keeps one entry
  per file (2026-11-18).
- [x] `utils/fetch_artifact.py`: the unused gh download helpers are gone
  (2026-11-18).

## Testing and analysis

- [x] **Test infrastructure** (2026-11-19): `aember test --coverage /
  --sanitize asan|tsan / --valgrind / --root`, `aember analyze`
  (clang-tidy, cppcheck, baseline), `aember mutate`, `aember lint --check`,
  warnings as errors, GitHub Actions CI. Coverage 65.5% → 84.4% of lines
  with the new tests (aember-init 2% → 64%, containerd 53% → 82% with a
  fake liblxc, netd 70% → 87%); 317 tests in 28 suites as root.
- [ ] Coverage gaps: aember-init's StartInitramfs (needs a real
  initramfs: the QEMU boots cover it), root-manager's switch (tested, but
  exec loses the counters), executables/aember/main.cpp, containerd's
  SquashFS/overlay mounts with the fake liblxc (needs root).
- [x] A fake liblxc (`SetLxcForTesting`, container-lifecycle-unittest):
  start with/without the init wrapper, graceful/forced/failed stop,
  adopt, exits and restarts, without root (2026-11-19).
- [ ] `MountManager::UnmountAll` forgets mounts whose unmount failed (it
  clears its list regardless).
- [x] `pid1::Finish` is tested in QEMU (2026-11-25): `aember systemtest`'s
  poweroff (`kill -USR2 1`) and reboot (`kill -TERM 1`) scenarios check the
  clean stop, PID 1's "shutting down" and the kernel's power-down/restart,
  and that QEMU ends by itself; in CI (job "System tests (QEMU)").
- [x] clang-tidy at 0 findings (strerror -> system_category().message(),
  NOLINT only where one thread runs); cppcheck keeps 10 style findings
  (raw loops, the deliberate local mutex for stop-token waits) in its
  baseline.
- [x] TLA+ model of start, restarts and shutdown (`spec/tla/`, CI job
  `tla`, 2026-11-25). It found that a restart pending at shutdown started
  its service again (or crashed PID 1 on a destroyed ServiceManager):
  fixed, `StopAll` cancels pending restarts.
- [x] `max_restarts` counts failures in a row for services too, like
  containers (2026-11-25, the user's decision): a run of 60 s
  (`ProcessConfig::stable_run`) starts the count over. Services used to
  count every failure since boot.
- [x] Regression labels (2026-11-25): past bug fixes from the history got a
  `Regression:` comment on the test that pins them; new tests for
  LinkUsable (namespace + carrier) and the CLI bugs
  (`aember-cli/tests/test_regressions.py`: guestfish error with exit 0,
  image sizing, compile_commands merge, `qemu build` clean condition).
- [x] Property tests (2026-11-25, `test-support/include/aember-test/`):
  config files, dependency resolver, ipc, DHCP, restart counting, CLI
  console reader. They found: numbers above 4294967295 in services.json and
  containers.json wrapped (`max_restarts: 4294967296` meant unlimited), and
  an empty DHCP option as the last byte of a reply aborted netd in -O0
  builds. A random dependency-graph generator with reference answers is
  ready for the parallel start.
- [x] Fuzzing (2026-11-25): `aember fuzz`, 8 targets in `fuzz/`, CI job
  `fuzz` (60 s each, corpus kept in the Actions cache). Found and fixed:
  netd's `value<int>()` on a float/huge number in network.json (undefined
  float-to-int), `Channel::Send` throwing on a string that isn't UTF-8.
  Fixed while writing the targets: containerd crashing on a "bridges" entry
  that isn't an object (`ParseBridges`), the status file left stale for good
  by a string that isn't UTF-8, `/proc/uptime` inf/nan cast to long, a
  "dns" string taken as one address.
- [x] Fuzz the `.aember` unpacker (`fuzz_package`, 2026-11-26).
- [x] `.aember` packages for containers and a parallel start graph in
  containerd (2026-11-26): containers.json lists names and dependencies,
  each container is a signed package installed once into
  `/var/lib/aember/<name>/`. x64 KVM boot, everything running: 3.53-3.63 s
  before, 2.82-3.23 s after (the three containers now come up within
  ~30 ms of each other instead of ~0.25 s apart).
- [ ] Services the same way: services.json only names and dependencies,
  each service's settings in its own folder (`config.json`).
- [ ] containerd's own dependency on netd (boards' services.json): only
  containers that need the network need it; with per-container
  dependencies on services, containers without network could start next
  to netd (~0.5 s at boot on x64).
- [ ] Fuzz more: `lxc.container.conf` handling, the debug shell's input.
- [ ] Past bugs still without a regression test: loopback brought up by
  PID 1 (`AemberInit::BringUpLoopback` is private; a QEMU system test or a
  root test in a private net namespace), containerd's pthread_atfork
  unblocking signals for container inits (liblxc forks without exec),
  container app output reaching the console (init wrapper inside a
  container with a /dev/console).
- [x] Mutation testing for every library and service (2026-11-25):
  `aember mutate -k <unit>` runs all suites of a unit against each mutant,
  root runs in a read-only sandbox; weekly in CI (`Mutation` workflow, one
  job per unit, scores + survivors in the summary). Scores (first run ->
  after the new tests):

  | Unit | Score | Unit | Score |
  |---|---|---|---|
  | aember-init | 38.8% | module-loader | 77.1% -> 88.6% |
  | child-supervisor | 81.5% -> 96.3% | mount-manager | 79.6% -> 83.3% |
  | config | 100% | netd | 69.7% -> 70.2% |
  | containerd | 70.7% -> 70.8% | netlink | 93.5% -> 95.7% |
  | device-health | 25.0% -> 100% | process | 100% |
  | ipc | 67.2% -> 69.0% | process-manager | 83.3% |
  | logging | 28.6% -> 71.4% | root | 100% |
  | module | 100% | root-manager | 64.5% (first run in the sandbox) |
  | service-manager | 71.0% -> 72.6% | shell | 82.9% |
  | signal | 78.6% -> 85.7% | watchdog | 68.6% -> 80.0% |

  All units: 73.9% -> about 76% (1166 mutants).
- [ ] Mutation survivors left to judge: aember-init (41: PID 1 paths only
  a booted system reaches; the QEMU system tests cover them, mutation
  doesn't run those), containerd (78) and netd (63) (daemon and netlink
  paths), ipc (18), service-manager (17, mostly double-checked state
  guards = equivalent), root-manager (27). process-manager's fork()
  `pid == 0` mutant "survives" because the test process itself execs the
  command and exits 0: a harness limit, not a gap.

## Licensing (task 12)

Phase 1 done (2026-11-26): `doc/licensing/` (inventory, notes for the legal
review, source offer, CLA draft), NOTICES.gz on every image, aember-auth and
aember-fleet-agent ported to Monocypher + mbedTLS backends (#7, #8 there).

- [ ] Phase 2, aember-resources v0.1.16 (exclusive build slot; one
  component and architecture at a time):
  - `kas/base.yml`: `INHERIT += "archiver create-spdx"`,
    `ARCHIVER_MODE[src] = "original"` + `ARCHIVER_MODE[diff] = "1"` (source +
    our patches per recipe), `SPDX_INCLUDE_SOURCES = "0"`; collect
    `deploy/spdx` and `deploy/sources` into the release assets
    (`aember-sources-<component>-<arch>.tar`, `*.spdx.json`).
  - `kas/recipes/sdk.yml`: `TOOLCHAIN_TARGET_TASK:append = " mbedtls-dev"`
    (meta-networking mbedtls 3.6.4); then aember-auth/aember-fleet default to
    `mbedtls` and drop their wolfSSL backends; wolfssl-dev leaves the SDK
    when nothing uses it.
  - connman: `PACKAGECONFIG:pn-connman` without `client` (no connmanctl, no
    GPL-3.0 readline); drop `connman-client` from aember-pkg-connman.
  - Optional Wi-Fi build without GPL: `PACKAGECONFIG`/config for
    wpa_supplicant and hostapd with `CONFIG_TLS=openssl` (OpenSSL 3,
    Apache-2.0), as a second package variant.
  - Rebuild order, x64 first: sdk, packages (connman, wifi, wifi-ap), then
    the other 7 architectures; the kernel, BusyBox and LXC only change by
    the archiver/SPDX classes (sstate reuse, new deploy outputs).
- [ ] Pi/USB images: put `LICENCE.broadcom` (already downloaded) and the
  systemd-boot notice on the boot partition; list them in NOTICES there.
- [ ] Containers carry their own notices (`/usr/share/licenses` inside each
  squashfs, written by aember-build-helper's pack), instead of aember's
  central list.
- [ ] Owner: legal review of `doc/licensing/LICENSING-NOTES.md`, then decide
  the LICENSE wording (Change Date, evaluation), the CLA, a trademark.
