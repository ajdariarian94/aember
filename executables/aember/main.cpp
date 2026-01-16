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

  aember::aember_init::AemberInit aember_init{argc, argv};

  aember_init.Start();

  return 0;
}
