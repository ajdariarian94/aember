# Performance of PID 1 and the services

What aember's own programs cost: size, boot time, CPU, wakeups, memory.
Measured 2026-11-26 on x64 (QEMU with KVM, 1 vCPU of a 16-core host) and
aarch64 (size only; QEMU's TCG says nothing about real boards), before the
parallel start of task 9 (`.aember`). Raspberry Pi numbers are still to be
taken on the board.

The first part is the method, so the numbers can be taken again after a
change; the second the results; the third what is left to do.

## Method

All commands run in the dev container, from the repository root.

**Size** (`tools/perf/size.py`, Bloaty McBloatface 1.1 in Docker,
`tools/perf/bloaty.Dockerfile`): VM size (what gets loaded) of
`usr/bin/aember`, `aember-netd`, `aember-containerd`, split by the library
the symbols come from. Needs a build with symbols:

```bash
aember app build --architecture x64-poky-linux        # once: vcpkg packages
tools/perf/variant.sh x64-poky-linux syms -Os syms     # Release flags, unstripped
tools/perf/size.py build/perf/x64-poky-linux-syms/install/bin/aember --top 20
```

`variant.sh ARCH NAME FLAGS` builds the same code with other optimisation
flags (`-O2`, `-O3`, ...) next to the normal build; the vcpkg packages are
reused from `aember app build` and keep the toolchain's flags.

**Boot, CPU, wakeups, memory** (`tools/perf/measure.py`, a system-test
scenario file):

```bash
aember app pack
aember systemtest --scenarios tools/perf/measure.py \
    --input runs=5 --input idle=30 --input label=now
# another build's binaries in a copy of the image:
aember systemtest --scenarios tools/perf/measure.py --input label=O3 \
    --input binaries=build/perf/x64-poky-linux-O3/tree
```

Per boot it reads, from the debug shell:

- when the kernel ran `/init` (dmesg) and when each entry reached running
  (`since_ms` in the status file, ms since the kernel started);
- CPU time of each process (all threads) from
  `/proc/<pid>/task/*/schedstat`, the scheduler's exact time on CPU:
  - "boot CPU" is the total when everything is running;
  - "idle" is the growth over the idle window, with the debug shell closed
    (PID 1 polls an open shell every 200 ms).
  - Not utime/stime: they are sampled at the timer tick and charged a
    1 s loop ten times its real cost.
- wakeups: voluntary + involuntary context switches over the idle window;
- RSS and PSS (`smaps_rollup`) at the end.

It writes `build/system-tests/<arch>/measure-<label>.json` with every
boot and the median, min and max. The 5-run spread of "all running" is
about ±50 ms on a busy host.

## Results

### Size

Release, stripped, VM size in KiB:

| Build | aember | netd | containerd | sum |
|---|---:|---:|---:|---:|
| x64 `-O3` (before) | 1201 | 676 | 714 | 2591 |
| x64 `-O2` | 1133 | 637 | 665 | 2434 |
| x64 `-Os` | 941 | 508 | 524 | 1974 |
| x64 `-Os`, no CLI11, fmt in netd (now) | 620 | 454 | 524 | 1597 |
| aarch64 `-O3` (before) | 1049 | 605 | 631 | 2285 |
| aarch64 `-Os` | 877 | 489 | 505 | 1871 |
| aarch64 now | 597 | 436 | 505 | 1539 |

These variant builds reuse the `-O3` vcpkg packages. A full build rebuilds
spdlog and fmt with `-Os` too, which takes the packed files further:

| File | Before | Now | Change |
|---|---:|---:|---:|
| x64 `usr/bin/aember` | 1235 KB | 518 KB | -58 % |
| x64 `usr/bin/aember-netd` | 702 KB | 350 KB | -50 % |
| x64 `usr/bin/aember-containerd` | 739 KB | 416 KB | -44 % |
| aarch64 `usr/bin/aember` | 1119 KB | 592 KB | -47 % |
| aarch64 `usr/bin/aember-netd` | 659 KB | 395 KB | -40 % |
| aarch64 `usr/bin/aember-containerd` | 659 KB | 461 KB | -30 % |

**Where the bytes went** (x64, `-O3`, before):

| Library | aember | netd | containerd |
|---|---:|---:|---:|
| CLI11 (two flags) | 279 KiB | - | - |
| aember's own code | 209 | 98 | 116 |
| nlohmann::json | 204 | 172 | 176 |
| spdlog | 158 | 154 | 158 |
| fmt (spdlog's) | 105 | 105 | 106 |
| `std::format` (next to fmt) | 86 | 86 | 86 |
| other libstdc++ templates | 60 | 20 | 25 |

**Now** (x64, `-Os`):

| Library | aember | netd | containerd |
|---|---:|---:|---:|
| spdlog | 154 KiB | 151 | 151 |
| nlohmann::json | 116 | 92 | 107 |
| fmt | 105 | 104 | 104 |
| aember's own code | 102 | 48 | 49 |
| `std::format` | 47 | 0 | 47 |

### Optimisation level: `-Os`, not `-O3`

Five boots each, x64 with KVM:

| Release flags | all running (ms) | PID 1 boot CPU (ms) | PID 1 idle CPU (ms/min) | PID 1 wakeups/s | PID 1 PSS (kB) |
|---|---:|---:|---:|---:|---:|
| `-O3` | 819 (792-863) | 420 | 216 | 4.5 | 2753 |
| `-O2` | 831 (817-853) | 442 | 225 | 4.6 | 2726 |
| `-Os` | 842 (790-894) | 461 | 225 | 4.7 | 2531 |

The differences are inside the spread. PID 1 and the services wait almost
all the time, and their boot CPU is kernel work (below), so `-O3`'s speed
buys nothing.

What `-O3` does cost is size. Compiler Explorer (`aember godbolt`) shows
why: on aarch64 a process-manager loop is 94 lines of assembly at `-O3`
(vectorised, unrolled) and 14 at `-Os`. Release is now `-Os` for every
target (`tools/toolchains/*.cmake`).

### Boot and idle, now

x64, KVM, five boots. Times are ms after the kernel started:

| | median | min-max |
|---|---:|---:|
| kernel runs `/init` | 654 | 643-700 |
| aember-netd running | 730 | |
| aember-echo running | 766 | |
| aember-stats running | 780 | |
| aember-containerd, aember-monitor running | 814 | 803-830 |

| Process | boot CPU | idle CPU | wakeups/s | RSS | PSS | threads |
|---|---:|---:|---:|---:|---:|---:|
| aember (PID 1) | 431 ms | 220 ms/min (0.37 %) | 4.6 | 4676 kB | 2154 kB | 7 |
| aember-netd | 19 ms | 9 ms/min | 0.5 | 4136 kB | 1632 kB | 2 |
| aember-containerd | 12 ms | 72 ms/min (0.12 %) | 1.1 | 5384 kB | 1950 kB | 1 |

PSS fell by 600, 310 and 310 kB from before (2754, 1940, 2260 kB).

**PID 1's boot CPU is the kernel's.** All 431 ms are system time in PID 1's
main thread: module loading (virtio-gpu, DRM, fbcon), mounts, the root
switch, and kernel messages on the serial console.
- `quiet` on the kernel command line takes it from 406 to 241 ms in QEMU.
- On a real 115200-baud UART, printk waits for every byte (about 87 µs),
  so the same messages cost much more wall time.
- Boards should boot with `quiet`. aember's own console lines (about 100
  at boot, 9 KB) are a second, smaller part.

**Idle wakeups of PID 1**, about 4.6 per second, per thread (20 s window,
debug shell closed):

| Thread | Wakeups | CPU | What |
|---|---|---|---|
| heartbeat | 2/s | 2 ms per beat | builds the status JSON, rewrites `/var/log/aember-status.json` |
| reaper | 1/s | 0.5 ms | `wait_for(1 s)` tick for the watchdog's liveness check, plus `CheckReadyTimeouts` |
| run loop | 1/s | ~0 | `wait_for(1 s)` tick, same reason |
| watchdog | 0.1/s | ~0 | feeds every timeout/3 (10 s) |

containerd wakes once a second (`poll` timeout 1000 ms) to reap and to ask
liblxc for each container's state: about 0.7 ms each time.

## Done in this round

- Release builds use `-Os` (all 8 toolchains).
- PID 1 parses its two flags (`--root`, `--chroot`) itself. CLI11 is gone
  from the binary (279 KiB), from vcpkg.json and from the licence
  inventory.
- netd and the libraries outside task 9's files use fmt, which spdlog
  already brings, not `std::format`. netd no longer carries both (86 KiB).
- The system-test harness:
  - no longer picks KVM for aarch64 on an x86 host (QEMU refused to
    start);
  - accepts a kernel message on a command's begin-marker line (it failed
    a boot now and then).

## After task 9: the follow-ups (2026-11-26)

Measured on main after task 9 (`main`) and with each step, same machine,
x64 QEMU with KVM, five boots each, 60 s idle (30 s for the last two),
`tools/perf/measure.py`. Since task 9 every boot runs three containers
(echo, stats, the Qt monitor), so the numbers aren't comparable with the
first round above.

| | main | idle work, tmpfs status, ext4, quiet (#212) | + containers next to netd |
|---|---:|---:|---:|
| PID 1 idle CPU | 2284 ms/min | 475 ms/min | 462 ms/min |
| PID 1 wakeups | 14.3/s | 3.5/s | 3.3/s |
| containerd idle CPU | 963 ms/min | 7.3 ms/min | 6.7 ms/min |
| containerd wakeups | 6.4/s | 0.13/s | 0.12/s |
| netd idle CPU | 80 ms/min | 71 ms/min | 68 ms/min |
| disk writes at idle | 16.8 MB/min | 16.6 MB/min | 224 KiB/min |
| PID 1 PSS | 2116 kB | 2047 kB | 2084 kB |
| all running (1 vCPU) | 3002 ms | 2908 ms | 3105 ms |

Stripped `-Os` binaries (`tools/perf/variant.sh`), bytes:

| | main | now |
|---|---:|---:|
| aember | 524168 | 466656 (-56 KiB) |
| aember-containerd | 531976 | 474488 (-56 KiB) |
| aember-netd | 347392 | 347392 |

What each step was:

1. **fmt instead of `std::format`** in aember-init, the status writer and
   containerd: no program carries both (-56 KiB each, above).
2. **Idle work.** Most of PID 1's and containerd's idle CPU was a scan of
   `/proc`: PID 1's reaper and containerd's loop read every process's
   `/proc/<n>/stat` on every wakeup to find their zombies, and the status
   writer read every `/proc/<n>/status` each heartbeat to count PID 1's
   children (the kernel has no `CONFIG_PROC_CHILDREN`); with the three
   containers that is ~70 processes. Now:
   - the scans use opendir/openat/read with a stack buffer (the stat
     line, not the status file);
   - PID 1's reaper scans only after a SIGCHLD, while an untracked zombie
     waits out its grace period, or every 30 s as a safety net, and
     ticks every 3 s otherwise (`Alive()` allows 10 s; every second while
     a ready timeout is pending); the run loop ticks every 3 s;
   - containerd's poll timeout is 10 s (every change wakes it anyway) and
     it scans only after a SIGCHLD;
   - the children count is reused for 10 s.
   PID 1's remaining wakeups are mostly the heartbeat (1 s).
3. **The status file on tmpfs:** `/run/aember/aember-status.json`,
   bind-mounted over `/var/log/aember-status.json` (containerd binds
   `/var/log` recursively, so the containers see it too). No more journal
   commits for it.
4. **ext4 without lazy init.** The 16 MB/min "at idle" was the kernel's
   `ext4lazyinit` zeroing inode tables in the first minute (32 MB on a
   fresh image). The images are now made by the guestfish appliance's
   `mke2fs -E lazy_itable_init=0,lazy_journal_init=0` (guestfish's own
   `mke2fs` call ignores those flags: its groups had no `ITABLE_ZEROED`).
   What is left, ~0.2 MB/min, is the file logs. The packed image is
   ~16 MB less sparse.
5. **`quiet`** on every board's kernel command line (`--append
   loglevel=7` for the full log). In QEMU it changed nothing measurable
   (all running 3105 ms with `quiet`, 3034 ms without, same spread):
   QEMU's serial port costs no time per byte. On a real 115200-baud UART
   each line waits ~90 µs per byte; measure it on the Pi.
6. **Containers next to aember-netd.** containerd no longer waits for netd
   (services.json); a container that needs a service lists it in
   containers.json `services`, PID 1 publishes the states. On one vCPU it
   gains nothing (netd and the container starts take turns on the CPU:
   3105 ms). With `--input smp=4` (a Pi 4 has four cores), same image,
   five boots each:

   | 4 vCPUs | aember-netd running | containers running | all running |
   |---|---:|---:|---:|
   | containerd after netd (old services.json) | 2328 | 2753-2759 | 2763 (2598-3025) |
   | containerd next to netd | 2424 | 2653-2660 | 2668 (2348-2877) |

   About 100 ms earlier, within the spread of a few runs: smaller than the
   0.5 s estimated before task 9.

`measure.py` gained `smp`, `append`, `overlay` (a tree copied over the
image, e.g. another services.json) and the disk writes.

## Left

1. **Bigger libraries.** spdlog (150 KiB per binary) and nlohmann::json
   (90-120 KiB) are the largest parts. A small logger on fmt alone would
   save most of spdlog's share, about 0.4 MB over the three programs.
2. **The heartbeat** is now PID 1's main periodic work (1 s: the status
   JSON, built and rewritten). Every 2-5 s, or only on change plus a
   slower liveness beat for aember-stats/aember-monitor, would take PID 1
   to well under 1 wakeup/s.
3. **aember-auth and the fleet agent** need the network: their
   aember-container.json could say `"services": ["aember-netd"]`
   (aember-build-helper would carry it into container.json). Today they
   retry (auth after 5 s).
4. **Raspberry Pi 4:** the same numbers on the board (`perf` is available
   there, and the UART cost is real), with 4 cores for the parallel start.
