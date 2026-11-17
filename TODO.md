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

- [ ] Containers ignore the graceful stop: `lxc shutdown` times out after
  5 s per container, then `stop` kills them. Set `lxc.signal.halt` to
  something the container init handles (SIGTERM for BusyBox init).
- [ ] At shutdown the SquashFS lower dir can't be unmounted (`EBUSY`): the
  overlay on top is only lazily detached (`MNT_DETACH`). Unmount the
  overlay for real first.
- [ ] `NetworkManager::Stop` waits up to one ping interval (the monitor
  thread sleeps); use a condition variable with the stop token.
- [ ] `ProcessManager::ScheduleRestart` detaches a thread that captures
  `this`; a restart pending at shutdown can touch a destroyed manager.
- [ ] Hardware **watchdog** support and recovery for PID 1 itself.

## Architecture

- [ ] **Move containers out of PID 1** (planned by the user). liblxc (4.8 MB)
  is linked into PID 1 and a crash there takes down init. Target: PID 1 only
  mounts, loads modules, reaps, handles signals and supervises; network,
  containers and the monitor become supervised services talking to PID 1
  over a small IPC (Unix socket, JSON). `ContainerManager` is the natural
  first service.
- [ ] **Fewer shell-outs to BusyBox** from PID 1 (squashfs `mount`,
  `ip link` fallback, `modprobe`): each is a fork/exec plus a hidden
  dependency (BusyBox `modprobe` doesn't read `modules.builtin`, see the
  `IsBuiltin` workaround). Use syscalls: `mount(2)` with `LOOP_CONFIGURE`,
  netlink, `finit_module(2)`.
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
- [ ] **aarch64 / Raspberry Pi** on real hardware. Builds, packs and boots in
  QEMU (`virt`, Cortex-A72) since 0.10.0 with aember-resources v0.1.2.
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
