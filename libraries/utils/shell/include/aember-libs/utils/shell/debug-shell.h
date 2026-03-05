/**
 * @file debug-shell.h
 * @author Arian Ajdari
 * @brief Lightweight debug shell utilities for Aember components
 * @version 0.1
 * @date 2026-03-05
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/logging/logging.h>

namespace aember::utils {

class DebugShell {
 public:
  DebugShell();

  virtual ~DebugShell() = default;

  bool CheckDebugShell();
  void SpawnDebugShell();
  void SilenceAemberInBackground();

 private:
  aember::utils::Logger log_;
};

}  // namespace aember::utils
