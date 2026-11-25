/**
 * @file common.h
 * @brief Helpers shared by aember's fuzz targets (see fuzz/CMakeLists.txt).
 */
#pragma once

#include <aember-libs/utils/logging/logging.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace fuzz {

/// The fuzz input as text.
inline std::string_view Text(const std::uint8_t* data, std::size_t size) {
  return {reinterpret_cast<const char*>(data), size};
}

/// A file only this fuzzer process uses (parsers that read a path).
inline std::string TempFile(std::string_view name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("aember-fuzz-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  return (dir / name).string();
}

inline void Write(const std::string& path, std::string_view text) {
  std::ofstream{path, std::ios::binary | std::ios::trunc} << text;
}

/// Log nothing: millions of runs, every parse error logged, is slow noise.
inline void Quiet() {
  static const bool once = [] {
    aember::utils::logging::set_console_logging(false);
    return true;
  }();
  (void)once;
}

/// A property the target checks failed: abort, libFuzzer saves the input.
[[noreturn]] inline void Fail(const char* what) {
  ::fprintf(stderr, "property failed: %s\n", what);
  std::abort();
}

}  // namespace fuzz
