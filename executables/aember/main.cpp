#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/aember-init/pid1.h>
#include <aember-libs/container-manager/containerd.h>
#include <aember-libs/network-manager/netd.h>
#include <aember-libs/utils/logging/logging.h>

#include <CLI/CLI.hpp>

#include <exception>
#include <string_view>

#include <unistd.h>

namespace pid1 = aember::aember_init::pid1;
using aember::utils::logging::Logger;

int main(int argc, char** argv) {
  // Multi-call: PID 1 starts its built-in daemons as `aember <role>` (same
  // file, so the code exists once and its pages are shared).
  if (argc > 1) {
    const std::string_view role{argv[1]};
    if (role == "netd") { return aember::network::RunNetd(); }
    if (role == "containerd") {
      return aember::container_manager::RunContainerd();
    }
  }

  // Before anything can throw: an uncaught exception must not kill PID 1
  // (the kernel panics when init exits).
  pid1::BlockInitSignals();
  pid1::InstallTerminateHandler();

  CLI::App app{"Aember"};

  bool root = false;
  bool chroot = false;

  app.add_flag("--root", root, "Start in root mode");
  app.add_flag("--chroot",
               chroot,
               "Root mode chrooted on a host: no kernel modules "
               "(aember app launch --chroot)");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    if (::getpid() != 1) { return app.exit(e); }
    Logger{"aember"}.warn("Ignoring bad arguments: {}", e.what());
  }

  auto action = pid1::ShutdownAction::kNone;
  try {
    aember::aember_init::AemberInit aember_init{root ? "aember" : "aember-init",
                                                {.chroot = chroot}};
    if (root) {
      aember_init.StartRoot();
    } else {
      aember_init.StartInitramfs();
    }
    action = aember_init.RequestedShutdown();
  } catch (const std::exception& e) {
    Logger{"aember"}.critical("Init failed: {}", e.what());
  } catch (...) { Logger{"aember"}.critical("Init failed"); }

  if (::getpid() != 1) { return EXIT_SUCCESS; }

  // PID 1 never returns from main.
  pid1::Finish(action);
}
