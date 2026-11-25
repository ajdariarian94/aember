# TLA+ models

Designs checked with [TLC](https://github.com/tlaplus/tlaplus), the TLA+
model checker: it explores every interleaving of a small instance and
checks invariants, action properties (what each single step may do) and
liveness (what must eventually happen). These are models of the design,
not proofs of the C++ code; where the code differs, it says so below.

```bash
spec/tla/check.sh                 # every model (*.cfg), ~15 s
spec/tla/check.sh DiamondLimited  # one model
```

TLC runs in Docker (`eclipse-temurin:21-jre`, `-Xmx3g`, `--memory 4g`);
`tla2tools.jar` v1.7.4 is downloaded once into `~/.cache/aember-tla` and
checked against its SHA-256. CI runs the same script (job `tla`). A model
whose `.cfg` says `EXPECTED TO FAIL` must find a violation: it keeps a bug
the model found reproducible in the model.

## StartGraph.tla: start, restart and shutdown

How aember starts the entries of `services.json` (today) and
`containers.json` (planned: the `.aember` work makes containers a
dependency graph too). This is the design reference for that work.

| Step | Code |
|------|------|
| `Load`: entries in a dependency cycle, or (transitively) behind one, are skipped; the rest waits | `DependencyResolver::ResolveStartOrder`, `ServiceManager::StartAll` |
| `Start(n)`: a waiting entry whose dependencies all run starts; independent entries in any order (= in parallel) | `ServiceManager::StartInternal` + `CheckDependencies` |
| `Ready(n)`: started (no ipc), or `ready` received, or `ready_timeout` passed | `MarkReady`, `CheckReadyTimeouts` |
| `Crash(n)`: the environment; bounded by `MaxCrashes` | |
| `OnFailure(n)`: restart unless `max_restarts` is used up (then `gave_up`) | `ProcessManager::ScheduleRestart`, `ContainerManager::NextRestart` |
| `RestartDue(n)`: after `restart_delay` it waits for its dependencies again | restart callback → `ServiceManager::Start` |
| `Stabilize(n)`: a run that lasts resets the count | `ProcessConfig::stable_run`, containerd's `kStableRun` (both 60 s) |
| `Shutdown`, `Stop(n)`, `Finish`: dependents stop first, pending restarts are cancelled, then `pid1::Finish` | `AemberInit::RequestShutdown`, `ServiceManager::StopAll`, `pid1::Finish` |

Constants: `Nodes`, `Deps` (`Deps[n]` = what n depends on),
`Restartable` (`restart_on_failure` / a `restart_policy` other than `no`),
`MaxRestarts` (0 = unlimited), `MaxCrashes`, `ResetOnStable` (TRUE: aember
today; FALSE only in `RestartDuringShutdown`),
`CancelRestartsOnShutdown`.

Properties:

- `SkippedNeverStart`, `SkipOnlyBad`: exactly the entries in or behind a
  cycle are skipped; one bad entry never stops the others.
- `DepsBeforeStart` (action): an entry starts only while all its
  dependencies run.
- `DependentsStopFirst` (action): at shutdown an entry stops only after
  all its dependents are down.
- `NoStartAfterShutdown`: nothing starts once shutdown began.
- `FinishedMeansDown`, `ShutdownCompletes`: shutdown always ends, with
  everything down.
- `EveryGoodEntryRuns` (liveness, weak fairness on what aember does
  itself): while aember is up, every good entry eventually runs, unless it
  or something it needs gave up.
- No deadlock (TLC's own check).

Models (`MCStartGraph.tla` defines the graphs):

| Model | Graph | Settings |
|-------|-------|----------|
| `Diamond` | netd ← containerd ← {stats, monitor} ← app | services today: unlimited restarts, no reset |
| `DiamondLimited` | same | containers: 1 restart in a row, reset after a stable run, monitor with `restart_policy: no` |
| `Cyclic` | a ← b; x ↔ y; z needs x and a | x, y, z skipped, a and b start |
| `RestartDuringShutdown` | Diamond | **expected to fail**: pending restarts not cancelled (aember before this fix) |

### Found by the model

**A restart pending at shutdown started its service again.** A service
crashed, its restart was scheduled for `restart_delay` later, and a
shutdown came in between: `StopAll` stopped everything else, then the
timer fired and started the service again during the shutdown. If it fired
after `service_manager_` was reset in `AemberInit::Stop`, the restart
callback dereferenced a null pointer in PID 1 (kernel panic instead of
reboot/poweroff). TLC's trace (`RestartDuringShutdown`): Load → netd starts
and runs → netd crashes → restart scheduled → Shutdown → others stop →
restart due → netd starts. Fixed: `ServiceManager::StopAll` first calls
`ProcessManager::CancelRestarts()` (stops and joins pending restart threads;
none is scheduled until `StartAll` calls `EnableRestarts()`), and an entry
that wasn't running is `Stopped` right away. Regression tests:
`ServiceManagerTest.StopAllCancelsAPendingRestart`,
`ProcessManagerTest.CancelRestartsStopsPendingAndNewOnes`.

### Where the code differs from the model

- **Containers have no dependency graph yet.** containerd starts the
  containers in file order, one at a time, and ignores `dependencies` in
  `containers.json`. The model is what the `.aember` work is to build.
- **A start under way** is stopped by `StopInternal` at once (SIGTERM);
  the model lets it finish (ready or failed) first. Both end stopped.
- **Readiness** (ipc `ready` or `ready_timeout`) is one step in the model.
