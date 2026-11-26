"""
Boot time, CPU, wakeups and memory of PID 1 and the services, measured in
QEMU (doc/performance.md). An aember system-test scenario file:

    aember systemtest --scenarios tools/perf/measure.py \\
        [--input runs=5] [--input idle=60] [--input binaries=DIR] \\
        [--input label=O3] [--input append="loglevel=7"] [--input smp=4]
        [--input overlay=DIR]

- runs: boots to measure (median and spread are reported).
- idle: seconds of idle measured after everything is running.
- append: extra kernel command line (e.g. loglevel=7 to undo the boards'
  `quiet`, to measure what the console costs).
- smp: virtual CPUs (QEMU -smp; default 1). A Raspberry Pi 4 has 4: on one
  vCPU, things that start in parallel only take turns.
- overlay: a directory copied over a copy of the image (e.g. another
  etc/aember/services.json), like binaries but without the check.
- binaries: a directory with usr/bin/aember, usr/bin/aember-netd,
  usr/bin/aember-containerd (e.g. another optimisation level's build,
  tools/perf/variant.sh): they replace the rootfs's (PID 1 after the root
  switch and the services; the initramfs stage keeps the packed binary).

Writes build/system-tests/<arch>/measure-<label>.json.

What is measured, per boot (all from the debug shell, BusyBox):
- boot: when the kernel ran /init (dmesg), and when each entry of the
  status file reached running (since_ms, ms since the kernel started).
- CPU: time on CPU (schedstat, all threads) of PID 1, netd and
  containerd when everything is up (= boot cost) and the delta over the
  idle window. The debug shell is closed meanwhile.
- wakeups: voluntary + involuntary context switches of all threads of
  each process over the idle window, per second.
- memory: VmRSS and Pss (smaps_rollup) and threads, at the end of idle.
- disk writes: KiB written to the root disk (/proc/diskstats, the disk
  that holds /) over the idle window, per minute: flash wear at idle.
"""

from __future__ import annotations

import json
import re
import statistics
import time
from pathlib import Path

from aember.steps.testing.system.scenarios import (
    System,
    boot_timeline,
    scenario,
    wait_all_running,
)
from aember.steps.testing.system.vm import Vm

PROCESSES = {"aember (PID 1)": "1", "aember-netd": None, "aember-containerd": None}
BINARIES = ("usr/bin/aember", "usr/bin/aember-netd", "usr/bin/aember-containerd")

# One line per process: pid run_ns ctxt rss_kb pss_kb threads. run_ns is
# the scheduler's exact time on CPU (/proc/<pid>/task/*/schedstat): utime
# and stime are sampled at the tick, which over-charges threads that wake
# on a timer aligned with it (a 1 s loop showed 10x its real cost).
PROBE = r"""
for p in {pids}; do
  ns=0; cs=0
  for t in /proc/$p/task/*; do
    ns=$((ns + $(cut -d' ' -f1 $t/schedstat)))
    for v in $(grep ctxt_switches $t/status | awk '{{print $2}}'); do
      cs=$((cs + v))
    done
  done
  rss=$(awk '/^VmRSS/ {{print $2}}' /proc/$p/status)
  pss=$(awk '/^Pss:/ {{print $2}}' /proc/$p/smaps_rollup 2>/dev/null)
  th=$(awk '/^Threads/ {{print $2}}' /proc/$p/status)
  echo "P $p $ns $cs $rss ${{pss:-0}} $th"
done
"""


def probe(vm: Vm, pids: dict[str, str]) -> dict[str, dict[str, int]]:
    out = vm.sh(PROBE.format(pids=" ".join(pids.values())).strip(), timeout=30)
    by_pid = {}
    for line in out.splitlines():
        parts = line.split()
        if parts[:1] == ["P"] and len(parts) == 7:
            pid, ns, cs, rss, pss, th = parts[1:]
            by_pid[pid] = {
                "run_ns": int(ns),
                "ctxt": int(cs),
                "rss_kb": int(rss),
                "pss_kb": int(pss),
                "threads": int(th),
            }
    return {name: by_pid[pid] for name, pid in pids.items() if pid in by_pid}


# KiB written to the disk that holds / (sectors are 512 bytes).
DISK_WRITES = r"""
d=$(awk '$2 == "/" {print $1}' /proc/mounts | sed 's|/dev/||; s|p\?[0-9]*$||' | tail -1)
awk -v d="$d" '$3 == d {print "W", $10 / 2}' /proc/diskstats
"""


def disk_written_kb(vm: Vm) -> float | None:
    out = vm.sh(DISK_WRITES.strip(), timeout=30)
    for line in out.splitlines():
        parts = line.split()
        if parts[:1] == ["W"] and len(parts) == 2:
            return float(parts[1])
    return None


def init_started_ms(vm: Vm) -> int | None:
    """When the kernel ran /init: its dmesg timestamp."""
    _, out = vm.run("dmesg | grep -m1 'Run /init as init process'")
    m = re.search(r"\[\s*(\d+\.\d+)\]", out)
    return round(float(m.group(1)) * 1000) if m else None


def one_boot(system: System, vm: Vm, idle: int) -> dict:
    vm.wait_ready(60)
    vm.open_shell()
    wait_all_running(system, vm)
    timeline = boot_timeline(vm.status())
    pids = {}
    for name, pid in PROCESSES.items():
        if pid is None:
            found = vm.pids(f"/usr/bin/{name}")
            if not found:
                raise vm.fail(f"{name} isn't running")
            pid = str(min(found))
        pids[name] = pid
    up = probe(vm, pids)
    written_before = disk_written_kb(vm)
    # Idle without the debug shell: PID 1 polls an open shell every 200 ms.
    vm.close_shell()
    time.sleep(idle)
    vm.open_shell()
    after = probe(vm, pids)
    written_after = disk_written_kb(vm)
    procs = {}
    for name in pids:
        a, b = up[name], after[name]
        procs[name] = {
            "boot_cpu_ms": round(a["run_ns"] / 1e6, 1),
            "idle_cpu_ms_per_min": round(
                (b["run_ns"] - a["run_ns"]) / 1e6 * 60 / idle, 1
            ),
            # Includes reopening the shell and the probe (a few switches):
            # small against the idle window.
            "wakeups_per_s": round((b["ctxt"] - a["ctxt"]) / idle, 2),
            "rss_kb": b["rss_kb"],
            "pss_kb": b["pss_kb"],
            "threads": b["threads"],
        }
    return {
        "init_started_ms": init_started_ms(vm),
        "all_running_ms": timeline[-1][0] if timeline else None,
        "timeline": [{"since_ms": t, "name": n} for t, n, _ in timeline],
        "processes": procs,
        "disk_writes_kb_per_min": (
            round((written_after - written_before) * 60 / idle, 1)
            if written_before is not None and written_after is not None
            else None
        ),
    }


def summarize(runs: list[dict]) -> dict:
    def stat(values: list[float]) -> dict:
        values = [v for v in values if v is not None]
        if not values:
            return {}
        return {
            "median": statistics.median(values),
            "min": min(values),
            "max": max(values),
        }

    out = {
        "init_started_ms": stat([r["init_started_ms"] for r in runs]),
        "all_running_ms": stat([r["all_running_ms"] for r in runs]),
        "entries_running_ms": {},
        "processes": {},
        "disk_writes_kb_per_min": stat([r.get("disk_writes_kb_per_min") for r in runs]),
    }
    names = {e["name"] for r in runs for e in r["timeline"]}
    for n in sorted(names):
        out["entries_running_ms"][n] = stat(
            [
                next((e["since_ms"] for e in r["timeline"] if e["name"] == n), None)
                for r in runs
            ]
        )
    for p in runs[0]["processes"]:
        out["processes"][p] = {
            k: stat([r["processes"][p][k] for r in runs])
            for k in runs[0]["processes"][p]
        }
    return out


@scenario("measure", "Boot time, CPU, wakeups and memory of PID 1 and the services")
def measure(system: System) -> None:
    runs = int(system.inputs.get("runs", "5"))
    idle = int(system.inputs.get("idle", "60"))
    label = system.inputs.get("label", "packed")
    append = system.inputs.get("append", "")
    smp = system.inputs.get("smp")
    extra = ["-smp", smp] if smp else []
    rootfs = None
    if "binaries" in system.inputs:
        tree = Path(system.inputs["binaries"]).resolve()
        missing = [b for b in BINARIES if not (tree / b).is_file()]
        if missing:
            raise AssertionError(f"binaries={tree}: missing {missing}")
        rootfs = system.image_with(tree, f"measure-{label}")
    elif "overlay" in system.inputs:
        tree = Path(system.inputs["overlay"]).resolve()
        rootfs = system.image_with(tree, f"measure-{label}")
    results = []
    for _ in range(runs):
        with system.boot(append=append, extra_args=extra, rootfs=rootfs) as vm:
            results.append(one_boot(system, vm, idle))
    report = {
        "label": label,
        "runs": runs,
        "idle_s": idle,
        "summary": summarize(results),
        "boots": results,
    }
    path = system.log_dir / f"measure-{label}.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    s = report["summary"]
    print(
        f"[measure] {label}: all running median "
        f"{s['all_running_ms'].get('median')} ms "
        f"({s['all_running_ms'].get('min')}-{s['all_running_ms'].get('max')}), "
        f"/init at {s['init_started_ms'].get('median')} ms"
    )
    for p, v in s["processes"].items():
        print(
            f"[measure]   {p}: boot cpu {v['boot_cpu_ms']['median']} ms, "
            f"idle {v['idle_cpu_ms_per_min']['median']} ms/min, "
            f"{v['wakeups_per_s']['median']} wakeups/s, "
            f"rss {v['rss_kb']['median']} kB, pss {v['pss_kb']['median']} kB, "
            f"{v['threads']['median']} threads"
        )
    print(
        f"[measure]   disk writes at idle: "
        f"{s['disk_writes_kb_per_min'].get('median')} KiB/min"
    )
    print(f"[measure] written to {path}")
