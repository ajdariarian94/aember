#include <aember-libs/aember-init/aember-init.h>
#include <aember-libs/aember-init/pid1.h>
#include <aember-libs/utils/logging/logging.h>

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#include <unistd.h>

namespace pid1 = aember::aember_init::pid1;
using aember::utils::logging::Logger;

namespace {

constexpr std::string_view kUsage =
    "Aember\n"
    "Usage: aember [--root] [--chroot]\n"
    "  --root    Start in root mode\n"
    "  --chroot  Root mode chrooted on a host: no kernel modules "
    "(aember app launch --chroot)\n";

struct Args {
  bool root = false;
  bool chroot = false;
  bool help = false;
  std::string bad;  // the first argument we don't know, if any
};

// Two flags: a hand-written loop instead of CLI11, which was 280 KiB of the
// 1.2 MiB PID 1 binary (doc/performance.md). The kernel passes init the
// command-line words it doesn't know itself, so as PID 1 anything else is
// ignored, not an error.
Args Parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--root") {
      args.root = true;
    } else if (arg == "--chroot") {
      args.chroot = true;
    } else if (arg == "--help" || arg == "-h") {
      args.help = true;
    } else if (args.bad.empty()) {
      args.bad = arg;
    }
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  // Before anything can throw: an uncaught exception must not kill PID 1
  // (the kernel panics when init exits).
  pid1::BlockInitSignals();
  pid1::InstallTerminateHandler();

  const Args args = Parse(argc, argv);
  if (::getpid() != 1) {
    if (args.help) {
      std::fputs(kUsage.data(), stdout);
      return EXIT_SUCCESS;
    }
    if (!args.bad.empty()) {
      std::fprintf(stderr,
                   "The following argument was not expected: %s\n"
                   "Run with --help for more information.\n",
                   args.bad.c_str());
      return 109;  // CLI11's exit code for extra arguments, as before
    }
  } else if (!args.bad.empty()) {
    Logger{"aember"}.warn("Ignoring bad arguments: {}", args.bad);
  }

  auto action = pid1::ShutdownAction::kNone;
  try {
    aember::aember_init::AemberInit aember_init{
        args.root ? "aember" : "aember-init", {.chroot = args.chroot}};
    if (args.root) {
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
