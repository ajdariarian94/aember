#include <aember-libs/aember-init/aember-init.h>

#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"Aember"};

  bool initramfs = false;
  bool root = false;

  app.add_flag("--initramfs", initramfs, "Start in initramfs mode");
  app.add_flag("--root", root, "Start in root mode");

  // Enforce exactly one mode
  app.require_option(1);

  CLI11_PARSE(app, argc, argv);

  if (initramfs) {
    aember::aember_init::AemberInit aember_init{"aember-init"};
    aember_init.StartInitramfs();
  } else if (root) {
    aember::aember_init::AemberInit aember_init{"aember"};
    aember_init.StartRoot();
  }

  return EXIT_SUCCESS;
}
