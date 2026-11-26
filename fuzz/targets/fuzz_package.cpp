/**
 * @file fuzz_package.cpp
 * @brief The .aember unpacker as containerd runs it on a package from
 *        anywhere: the input is a package's payload (an xz stream of the
 *        tar; the seeds are real ones), sealed into a package and
 *        installed (unsigned allowed) into a scratch directory; and the
 *        input as a tar straight into the tar reader.
 *        Properties: never crashes, hangs or throws; an install that
 *        succeeds left the three files and package.json, nothing else; one
 *        that fails left the directory as it was.
 */
#include <aember-libs/utils/package/install.h>
#include <aember-libs/utils/package/writer.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "common.h"

namespace {

namespace fs = std::filesystem;
namespace pkg = aember::utils::package;

class NullSink final : public pkg::Sink {
 public:
  std::expected<void, std::string> Begin(std::string_view,
                                         std::uint64_t) override {
    return {};
  }
  std::expected<void, std::string> Write(
      std::span<const std::uint8_t>) override {
    return {};
  }
  std::expected<void, std::string> End() override { return {}; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  fuzz::Quiet();
  const std::span<const std::uint8_t> input{data, size};

  // The bytes as the tar itself.
  {
    NullSink sink;
    pkg::TarReader reader{sink};
    if (reader.Feed(input)) std::ignore = reader.Finish();
  }

  // The bytes as a package's payload, installed.
  static const fs::path dir = fs::path{fuzz::TempFile("package")};
  const auto inbox = dir / "inbox";
  const auto root = dir / "root";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(inbox);
  const auto package = (inbox / "app.aember").string();
  {
    const auto bytes = pkg::Seal({input.begin(), input.end()}, std::nullopt);
    std::ofstream out{package, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  pkg::InstallOptions options;
  options.root = root.string();
  options.policy.allow_unsigned = true;
  const auto result = pkg::Install(package, options);

  std::vector<std::string> left;
  if (fs::is_directory(root, ec)) {
    for (const auto& e : fs::directory_iterator{root, ec}) {
      left.push_back(e.path().filename().string());
    }
  }
  if (result) {
    if (left != std::vector<std::string>{"app"}) fuzz::Fail("extra entries");
    for (const auto* f : {"config.json",
                          "lxc.container.conf",
                          "image.squashfs",
                          "package.json"}) {
      if (!fs::is_regular_file(root / "app" / f)) fuzz::Fail("a file missing");
    }
    if (fs::exists(package)) fuzz::Fail("the package wasn't removed");
  } else if (!left.empty()) {
    fuzz::Fail("a failed install left something behind");
  }
  return 0;
}
