#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/aember-init/pid1.h>

#include <CLI/CLI.hpp>

#include <cstdio>
#include <exception>

#include <unistd.h>

namespace pid1 = aember::aember_init::pid1;

int main(int argc, char** argv) {
  // Before anything can throw: an uncaught exception must not kill PID 1
  // (the kernel panics when init exits).
  pid1::BlockInitSignals();
  pid1::InstallTerminateHandler();

  CLI::App app{"Aember"};

  bool root = false;

  app.add_flag("--root", root, "Start in root mode");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    if (::getpid() != 1) { return app.exit(e); }
    std::fprintf(stderr, "aember: ignoring bad arguments: %s\n", e.what());
  }

  auto action = pid1::ShutdownAction::kNone;
  try {
    aember::aember_init::AemberInit aember_init{root ? "aember"
                                                     : "aember-init"};
    if (root) {
      aember_init.StartRoot();
    } else {
      aember_init.StartInitramfs();
    }
    action = aember_init.RequestedShutdown();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "aember: init failed: %s\n", e.what());
  } catch (...) { std::fprintf(stderr, "aember: init failed\n"); }

  if (::getpid() != 1) { return EXIT_SUCCESS; }

  // PID 1 never returns from main.
  pid1::Finish(action);
}
