---------------------------- MODULE StartGraph ----------------------------
(***************************************************************************)
(* How aember starts, restarts and stops the entries of services.json and  *)
(* (planned, task 9) containers.json: a dependency graph, started in       *)
(* parallel.                                                               *)
(*                                                                         *)
(* - Load: entries in a dependency cycle, or depending (transitively) on   *)
(*   one, are skipped and never start; every other entry waits.            *)
(* - Any waiting entry whose dependencies all run may start. Interleaving  *)
(*   models parallelism: independent entries start in any order.           *)
(* - A start ends in "running" (or "failed"); with ipc the entry runs      *)
(*   once it reports ready or its ready_timeout passes (one step here).   *)
(* - A running entry may crash at any time (the environment). With         *)
(*   restart_on_failure it is restarted after restart_delay unless it has  *)
(*   used up max_restarts; a restart waits again for its dependencies.    *)
(*   Dependents of a crashed entry keep running.                           *)
(* - Shutdown (SIGTERM/SIGINT/SIGUSR1/SIGUSR2 to PID 1) stops every entry *)
(*   only after all its dependents stopped (reverse dependency order);     *)
(*   pending restarts are cancelled; then pid1::Finish.                    *)
(*                                                                         *)
(* Code: libraries/service-manager (ServiceManager, DependencyResolver),   *)
(* libraries/process-manager (restarts), libraries/aember-init (shutdown), *)
(* services/containerd (container restarts). README.md lists where the     *)
(* code differs from this model.                                           *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets

CONSTANTS
    Nodes,          \* the entries (services or containers)
    Deps,           \* Deps[n]: the names n depends on (a subset of Nodes)
    Restartable,    \* entries with restart_on_failure / a restart_policy
    MaxRestarts,    \* max_restarts / max_restart_attempts; 0 = unlimited
    MaxCrashes,     \* how many crashes the environment may cause in all
    ResetOnStable,  \* TRUE: a run that lasts resets the count (containers)
    CancelRestartsOnShutdown \* TRUE: pending restarts die with shutdown

ASSUME Deps \in [Nodes -> SUBSET Nodes]
ASSUME Restartable \subseteq Nodes
ASSUME MaxRestarts \in Nat /\ MaxCrashes \in Nat
ASSUME ResetOnStable \in BOOLEAN /\ CancelRestartsOnShutdown \in BOOLEAN

(* Transitive dependencies: Reach(n) are all entries n needs, directly or  *)
(* not. The fixpoint of at most |Nodes| steps.                             *)
RECURSIVE ReachN(_, _)
ReachN(S, k) == IF k = 0 THEN S
                ELSE ReachN(S \cup UNION {Deps[m] : m \in S}, k - 1)
Reach(n) == ReachN(Deps[n], Cardinality(Nodes))

InCycle(n) == n \in Reach(n)
Skipped == {n \in Nodes : InCycle(n) \/ \E d \in Reach(n) : InCycle(d)}
Good == Nodes \ Skipped
Dependents(n) == {m \in Nodes : n \in Deps[m]}

States == {"stopped", "skipped", "waiting", "starting", "running",
           "failed", "restarting", "gave_up", "stopping"}
Phases == {"boot", "up", "shutdown", "finished"}

VARIABLES
    st,         \* st[n]: the entry's state
    restarts,   \* restarts[n]: restarts in a row
    stable,     \* stable[n]: has run long enough to count as healthy
    crashes,    \* crashes so far (bounded by MaxCrashes)
    phase,      \* boot -> up -> shutdown -> finished
    startedLate \* some entry started after shutdown began (must stay FALSE)

vars == <<st, restarts, stable, crashes, phase, startedLate>>

TypeOK ==
    /\ st \in [Nodes -> States]
    /\ restarts \in [Nodes -> Nat]
    /\ stable \in [Nodes -> BOOLEAN]
    /\ crashes \in 0..MaxCrashes
    /\ phase \in Phases
    /\ startedLate \in BOOLEAN

Init ==
    /\ st = [n \in Nodes |-> "stopped"]
    /\ restarts = [n \in Nodes |-> 0]
    /\ stable = [n \in Nodes |-> FALSE]
    /\ crashes = 0
    /\ phase = "boot"
    /\ startedLate = FALSE

(* StartAll: the resolver drops cycles and what depends on them; the rest  *)
(* waits for its dependencies.                                             *)
Load ==
    /\ phase = "boot"
    /\ phase' = "up"
    /\ st' = [n \in Nodes |-> IF n \in Skipped THEN "skipped" ELSE "waiting"]
    /\ UNCHANGED <<restarts, stable, crashes, startedLate>>

DepsRunning(n) == \A d \in Deps[n] : st[d] = "running"

(* CheckDependencies + process_manager.Start / StartContainer. Restarts    *)
(* that fire during shutdown reach this too unless they were cancelled.    *)
Start(n) ==
    /\ st[n] = "waiting"
    /\ DepsRunning(n)
    /\ phase \in {"up", "shutdown"}
    /\ phase = "shutdown" => ~CancelRestartsOnShutdown
    /\ st' = [st EXCEPT ![n] = "starting"]
    /\ startedLate' = (startedLate \/ phase = "shutdown")
    /\ UNCHANGED <<restarts, stable, crashes, phase>>

(* Spawned (no ipc) / MarkReady / ready timeout. *)
Ready(n) ==
    /\ st[n] = "starting"
    /\ st' = [st EXCEPT ![n] = "running"]
    /\ stable' = [stable EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<restarts, crashes, phase, startedLate>>

(* The environment: a start or a running entry fails. *)
Crash(n) ==
    /\ st[n] \in {"starting", "running"}
    /\ crashes < MaxCrashes
    /\ crashes' = crashes + 1
    /\ st' = [st EXCEPT ![n] = "failed"]
    /\ restarts' = IF ResetOnStable /\ stable[n]
                   THEN [restarts EXCEPT ![n] = 0] ELSE restarts
    /\ UNCHANGED <<stable, phase, startedLate>>

(* kStableRun (containers): a run that lasts resets the count. *)
Stabilize(n) ==
    /\ ResetOnStable
    /\ st[n] = "running"
    /\ ~stable[n]
    /\ stable' = [stable EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<st, restarts, crashes, phase, startedLate>>

(* ScheduleRestart / NextRestart, on the exit. *)
OnFailure(n) ==
    /\ st[n] = "failed"
    /\ phase = "up"
    /\ IF n \in Restartable /\ (MaxRestarts = 0 \/ restarts[n] < MaxRestarts)
       THEN /\ st' = [st EXCEPT ![n] = "restarting"]
            /\ restarts' = [restarts EXCEPT ![n] = @ + 1]
       ELSE /\ st' = [st EXCEPT ![n] = "gave_up"]
            /\ UNCHANGED restarts
    /\ UNCHANGED <<stable, crashes, phase, startedLate>>

(* restart_delay passed: start it again (it waits for its dependencies). *)
RestartDue(n) ==
    /\ st[n] = "restarting"
    /\ phase \in {"up", "shutdown"}
    /\ phase = "shutdown" => ~CancelRestartsOnShutdown
    /\ st' = [st EXCEPT ![n] = "waiting"]
    /\ UNCHANGED <<restarts, stable, crashes, phase, startedLate>>

(* SIGTERM/SIGINT (reboot), SIGUSR2 (power off), SIGUSR1 (halt). *)
Shutdown ==
    /\ phase = "up"
    /\ phase' = "shutdown"
    /\ UNCHANGED <<st, restarts, stable, crashes, startedLate>>

Down == {"stopped", "skipped", "gave_up"}

(* StopAll: dependents first. An entry that isn't running (waiting,       *)
(* failed, restarting) just drops out. Stopping is synchronous: SIGTERM,  *)
(* stop_timeout, SIGKILL.                                                  *)
Stop(n) ==
    /\ phase = "shutdown"
    /\ st[n] \notin Down \cup {"starting"}
    /\ \A m \in Dependents(n) : st[m] \in Down
    \* Without cancelling, a pending restart's timer is still armed: only
    \* RestartDue ends it (and may then start the entry again).
    /\ st[n] = "restarting" => CancelRestartsOnShutdown
    /\ st' = [st EXCEPT ![n] = "stopped"]
    /\ UNCHANGED <<restarts, stable, crashes, phase, startedLate>>

(* A start that is under way finishes first (ready or failed), then stops. *)

(* pid1::Finish: SIGTERM/SIGKILL to what's left, sync, reboot(2). *)
Finish ==
    /\ phase = "shutdown"
    /\ \A n \in Nodes : st[n] \in Down
    /\ phase' = "finished"
    /\ UNCHANGED <<st, restarts, stable, crashes, startedLate>>

Done == phase = "finished" /\ UNCHANGED vars

Next ==
    \/ Load \/ Shutdown \/ Finish \/ Done
    \/ \E n \in Nodes :
        Start(n) \/ Ready(n) \/ Crash(n) \/ Stabilize(n) \/ OnFailure(n)
        \/ RestartDue(n) \/ Stop(n)

(* Fair: everything aember does by itself. Not fair: crashes and the      *)
(* shutdown signal (the outside world), and Stabilize (runs may be short). *)
Fairness ==
    /\ WF_vars(Load) /\ WF_vars(Finish)
    /\ \A n \in Nodes :
        /\ WF_vars(Start(n)) /\ WF_vars(Ready(n)) /\ WF_vars(OnFailure(n))
        /\ WF_vars(RestartDue(n)) /\ WF_vars(Stop(n))

Spec == Init /\ [][Next]_vars /\ Fairness

---------------------------------------------------------------------------
(* Safety *)

(* Entries in or behind a cycle never start; the others are never skipped. *)
SkippedNeverStart ==
    phase # "boot" => \A n \in Nodes : (st[n] = "skipped") <=> (n \in Skipped)
SkipOnlyBad == \A n \in Good : st[n] # "skipped"

(* Nothing starts once shutdown began. *)
NoStartAfterShutdown == ~startedLate

(* Finish only when everything is down. *)
FinishedMeansDown == phase = "finished" => \A n \in Nodes : st[n] \in Down

(* Action properties: an entry starts only when all its dependencies run; *)
(* an entry stops only when all its dependents are down.                   *)
DepsBeforeStart ==
    [][\A n \in Nodes :
        (st[n] = "waiting" /\ st'[n] = "starting") => DepsRunning(n)]_vars
DependentsStopFirst ==
    [][\A n \in Nodes :
        (phase = "shutdown" /\ st[n] # "stopped" /\ st'[n] = "stopped")
            => \A m \in Dependents(n) : st[m] \in Down]_vars

---------------------------------------------------------------------------
(* Liveness *)

(* n is blocked for good: it, or something it needs, gave up. *)
Blocked(n) == \E d \in Reach(n) \cup {n} : st[d] = "gave_up"

(* While aember stays up, every good entry eventually runs (crashes are   *)
(* bounded, so eventually it stays up) unless something it needs gave up. *)
EveryGoodEntryRuns ==
    \A n \in Good :
        [](phase = "up" => <>(phase # "up" \/ st[n] = "running" \/ Blocked(n)))

(* A shutdown always completes. *)
ShutdownCompletes == (phase = "shutdown") ~> (phase = "finished")

=============================================================================
