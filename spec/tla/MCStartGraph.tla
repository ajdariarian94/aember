--------------------------- MODULE MCStartGraph ---------------------------
(* The graphs TLC checks StartGraph on (each .cfg picks one).               *)
EXTENDS StartGraph

(* A diamond: netd; containerd needs netd; stats and monitor need           *)
(* containerd; app needs stats and monitor.                                 *)
Diamond == {"netd", "containerd", "stats", "monitor", "app"}
DiamondDeps == [n \in Diamond |->
    CASE n = "netd"       -> {}
      [] n = "containerd" -> {"netd"}
      [] n = "stats"      -> {"containerd"}
      [] n = "monitor"    -> {"containerd"}
      [] n = "app"        -> {"stats", "monitor"}]

(* A cycle (x <-> y), an entry behind it (z) and two good ones.             *)
Cyclic == {"a", "b", "x", "y", "z"}
CyclicDeps == [n \in Cyclic |->
    CASE n = "a" -> {}
      [] n = "b" -> {"a"}
      [] n = "x" -> {"y"}
      [] n = "y" -> {"x"}
      [] n = "z" -> {"x", "a"}]

DiamondRestartable == Diamond \ {"monitor"}
=============================================================================
