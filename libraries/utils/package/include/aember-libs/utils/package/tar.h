/**
 * @file tar.h
 * @author Arian Ajdari
 * @brief The tar inside a `.aember` payload: ustar, regular files only,
 *        exactly config.json, lxc.container.conf and image.squashfs in
 *        that order. Read as a stream, so the image never has to fit in
 *        memory; what it holds goes to a Sink.
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace aember::utils::package {

inline constexpr std::size_t kTarBlock = 512;

struct TarMember {
  std::string name;
  std::uint64_t size{0};
};

/// One ustar header block. nullopt: an all-zero block (the end). Errors for
/// anything but a regular file with a plain name, a bad checksum or size.
std::expected<std::optional<TarMember>, std::string> ParseTarHeader(
    std::span<const std::uint8_t, kTarBlock> block);

/// The header of a regular file @p name (< 100 bytes) of @p size bytes:
/// mode 0644, uid/gid 0, mtime 0 (packages are reproducible).
std::array<std::uint8_t, kTarBlock> TarHeader(std::string_view name,
                                              std::uint64_t size);

/// Where the members go.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual std::expected<void, std::string> Begin(std::string_view name,
                                                 std::uint64_t size) = 0;
  virtual std::expected<void, std::string> Write(
      std::span<const std::uint8_t> data) = 0;
  virtual std::expected<void, std::string> End() = 0;
};

/// Feed the tar's bytes in pieces of any size; Finish() at the end.
class TarReader {
 public:
  explicit TarReader(Sink& sink) : sink_(sink) {}

  std::expected<void, std::string> Feed(std::span<const std::uint8_t> data);
  /// The tar ended properly (end blocks after the three members).
  std::expected<void, std::string> Finish();

 private:
  std::expected<void, std::string> Header();

  enum class State { kHeader, kData, kPadding, kEnd };

  Sink& sink_;
  State state_{State::kHeader};
  std::array<std::uint8_t, kTarBlock> block_{};
  std::size_t block_fill_{0};
  std::uint64_t left_{0};     ///< data or padding bytes still to come
  std::uint64_t padding_{0};  ///< padding after the current member's data
  std::size_t members_{0};    ///< members seen
};

}  // namespace aember::utils::package
