/**
 * @file start-graph.h
 * @author Arian Ajdari
 * @brief Which containers may start now: containers.json's dependency
 *        graph, as spec/tla/StartGraph.tla designs it.
 *
 *  - At load, entries in a dependency cycle, behind one, with a dependency
 *    that isn't in the graph, or listed twice are skipped (with a reason);
 *    the rest are unaffected.
 *  - A wanted entry starts once all its dependencies run; entries that
 *    don't depend on each other start together (containerd starts each
 *    in its own thread).
 *  - A restart wants the entry again: it waits for its dependencies too.
 *  - Stopping goes dependents first.
 *
 * Pure bookkeeping (no threads, no LXC): containerd's loop drives it.
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace aember::containerd {

class StartGraph {
 public:
  struct Node {
    std::string name;
    std::vector<std::string> dependencies;
  };

  explicit StartGraph(const std::vector<Node>& nodes);

  /// Entries skipped at load: name -> why. They never start.
  [[nodiscard]] const std::map<std::string, std::string>& Skipped() const {
    return skipped_;
  }
  /// The entries that may start (everything but the skipped), in order.
  [[nodiscard]] const std::vector<std::string>& Names() const { return names_; }
  [[nodiscard]] bool Has(const std::string& name) const;

  /// Start @p name as soon as its dependencies run (all of them at boot; one
  /// again for a restart). Ignored for skipped or unknown names.
  void Want(const std::string& name);
  /// Not any more (a stop was requested).
  void Unwant(const std::string& name);
  /// The wanted entries whose dependencies all run; they are starting now.
  std::vector<std::string> TakeReady();
  /// A start finished: @p running whether it came up. An exit later:
  /// SetRunning(name, false).
  void SetRunning(const std::string& name, bool running);

  /// Every dependency of @p name runs.
  [[nodiscard]] bool DependenciesRun(const std::string& name) const;
  [[nodiscard]] bool IsRunning(const std::string& name) const;
  [[nodiscard]] bool IsWanted(const std::string& name) const;
  [[nodiscard]] bool IsStarting(const std::string& name) const;
  /// A start is in progress.
  [[nodiscard]] bool Busy() const { return !starting_.empty(); }

  /// Every entry, dependents before their dependencies (stop order).
  [[nodiscard]] std::vector<std::string> StopOrder() const;
  /// The longest chain of dependencies: the fewest start waves.
  [[nodiscard]] int Depth() const;

 private:
  std::vector<std::string> names_;  ///< good entries, file order
  std::map<std::string, std::vector<std::string>> deps_;
  std::map<std::string, std::string> skipped_;
  std::set<std::string> wanted_;
  std::set<std::string> starting_;
  std::set<std::string> running_;
};

}  // namespace aember::containerd
