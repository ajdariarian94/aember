#include <aember-libs/aember-init/aember-init.h>

int main(int /*argc*/, char** /*argv*/) {
  aember::aember_init::AemberInit aember_init{};

  aember_init.Start();

  return 0;
}
