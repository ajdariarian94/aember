#include <aember-libs/aember-init/aember-init.h>

#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"Aember"};

  bool root = false;

  app.add_flag("--root", root, "Start in root mode");

  CLI11_PARSE(app, argc, argv);

  if (root) {
    aember::aember_init::AemberInit aember_init{"aember"};
    aember_init.StartRoot();
  } else {
    aember::aember_init::AemberInit aember_init{"aember-init"};
    aember_init.StartInitramfs();
  }

  return EXIT_SUCCESS;
}
