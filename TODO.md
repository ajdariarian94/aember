# TODO — aember

Ideas and findings collected so far. **Nothing here is approved yet:** implement
an item only when the user says so. Related items for the low-level pieces
(kernel, BusyBox, LXC, SDK) are in `../aember-resources/TODO.md`.

**Agreed order (2026-11-15):** 1. aarch64 working end to end, 2. arm32,
3. then size optimization (kernel trimming etc.).

## Boot time and the debug shell

- [ ] **Remove the 5 s "press 'd' for debug shell" wait.** It's about half of
  the ~10 s boot (log: 18:48:06 → 18:48:11 before mounting continues).
  Replace it with a combination of:
  - [ ] **Background key listener:** watch the console from the existing
    `epoll` loop (or a thread) instead of blocking. Boot continues
    immediately; pressing `d` at any time spawns a supervised shell on the
    console.
  - [ ] **Emergency shell on failure:** if the root mount or `pivot_root`
    fails, drop into a shell instead of hanging or panicking.
  - [ ] **Kernel command line flags**, read from `/proc/cmdline`:
    `aember.shell=1` (shell at start) and `aember.shell=initramfs` (stop
    before the pivot, for root mount problems). Add
    `aember app launch --debug-shell` to append it in QEMU; on a Raspberry Pi
    it goes into `cmdline.txt`.
  - [ ] Optional: in Debug builds, a console shell/getty as a normal entry
    in `services.json` (restarted when it exits), absent in Release.

## PID 1 robustness

- [ ] **PID 1 must never die:** top-level `try`/`catch` around the run loop
  that logs and keeps reaping; parse configs without exceptions (nlohmann
  `json::parse(..., nullptr, false)` / `std::expected`). An uncaught
  exception is a kernel panic.
- [ ] **Re-enable and add tests.** Several libraries have their `tests/`
  subdirectory commented out in CMake. A unit test would have caught the
  `ModuleLoader` bug fixed in d841a7e (`std::ranges::all_of`
  stopped at the first failing module).
- [ ] **Investigate `RemoveChild: unknown pid=<n>`** warnings at boot: a child
  exits that `ChildSupervisor` doesn't track (probably LXC helper processes).
  Possible reaping race.
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
- [ ] `BuildStep` (`steps/cmake/build.py`): give `--parallel` a job count
  (WSL crashes when the build uses every core).
- [ ] **arm32 (ARMv7, `arm-poky-linux`)** after aarch64 works: common on
  embedded boards (i.MX6, STM32MP1, older Pis). Needs a machine/kas file,
  kernel and BusyBox fragments, LXC conf and assets in aember-resources, plus
  a toolchain file, vcpkg triplet, `config/` entries and a QEMU profile
  (`virt`, Cortex-A15/A7) here. Own branch, after aarch64 is merged.
- [ ] Docs and a small example appliance, so others can try it in an
  afternoon.

## Smaller cleanups / known issues

- [ ] `cli/qemu/build.py`: `if build_tests or BuildType.debug:` is always
  true, so every `aember qemu build` starts with `CleanStep`.
- [ ] Dev Dockerfile has Yocto-era leftovers (GNU tar 1.34 downgrade,
  gcc-12/14 PPA, gcc cross packages); drop them on the next image rebuild.
- [ ] `aember changelog` compares `main..HEAD`, so it finds nothing on
  `main`; let it take the last tag as base (`--since v0.9.0`).
- [ ] `aember release` has no upload step; add an `aember publish` like
  aember-resources' (gh release create/upload at the pushed HEAD).
- [ ] `utils/fetch_artifact.py` still has the old gh-based download helpers;
  remove what's no longer used.
