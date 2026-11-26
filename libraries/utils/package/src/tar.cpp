/**
 * @file tar.cpp
 * @author Arian Ajdari
 * @brief The `.aember` payload's tar (see tar.h).
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/package.h>
#include <aember-libs/utils/package/tar.h>

#include <algorithm>
#include <cstdio>
#include <ranges>

namespace aember::utils::package {

namespace {

constexpr std::array<std::string_view, 3> kMembers{
    kConfigFile, kLxcFile, kImageFile};

/// A NUL- (or space-) terminated octal field; nullopt if it isn't one.
std::optional<std::uint64_t> Octal(std::span<const std::uint8_t> field) {
  std::uint64_t value = 0;
  bool digits = false;
  std::size_t i = 0;
  while (i < field.size() && field[i] == ' ') ++i;  // leading spaces
  for (; i < field.size(); ++i) {
    const auto c = field[i];
    if (c == 0 || c == ' ') break;
    if (c < '0' || c > '7') return std::nullopt;
    if (value > (UINT64_MAX >> 3U)) return std::nullopt;
    value = (value << 3U) | static_cast<std::uint64_t>(c - '0');
    digits = true;
  }
  for (; i < field.size(); ++i) {  // only terminators after the digits
    if (field[i] != 0 && field[i] != ' ') return std::nullopt;
  }
  if (!digits) return std::nullopt;
  return value;
}

void PutOctal(std::span<std::uint8_t> field, std::uint64_t value) {
  // width-1 digits, zero padded, then NUL.
  const std::size_t width = field.size() - 1;
  for (std::size_t i = 0; i < width; ++i) {
    field[width - 1 - i] = static_cast<std::uint8_t>('0' + (value & 7U));
    value >>= 3U;
  }
  field[width] = 0;
}

std::uint64_t Checksum(std::span<const std::uint8_t, kTarBlock> block) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kTarBlock; ++i) {
    sum += (i >= 148 && i < 156) ? ' ' : block[i];
  }
  return sum;
}

std::uint64_t Limit(std::string_view name) {
  return name == kImageFile ? kMaxImageSize : kMaxTextSize;
}

}  // namespace

std::expected<std::optional<TarMember>, std::string> ParseTarHeader(
    std::span<const std::uint8_t, kTarBlock> block) {
  if (std::ranges::all_of(block, [](std::uint8_t b) { return b == 0; })) {
    return std::optional<TarMember>{};
  }
  const auto checksum = Octal(block.subspan(148, 8));
  if (!checksum || *checksum != Checksum(block)) {
    return std::unexpected("tar header checksum doesn't match");
  }
  const std::string_view magic{reinterpret_cast<const char*>(&block[257]), 8};
  if (magic != std::string_view{"ustar\0"
                                "00",
                                8}) {
    return std::unexpected("not a ustar header");
  }
  if (block[156] != '0' && block[156] != 0) {
    return std::unexpected("tar member is not a regular file");
  }
  if (block[345] != 0) return std::unexpected("tar member with a prefix");
  const auto* name_begin = reinterpret_cast<const char*>(block.data());
  const auto name_end = std::find(name_begin, name_begin + 100, '\0');
  TarMember member;
  member.name.assign(name_begin, name_end);
  if (member.name.empty() || member.name.find('/') != std::string::npos) {
    return std::unexpected("tar member without a plain name");
  }
  const auto size = Octal(block.subspan(124, 12));
  if (!size) return std::unexpected("tar member size is not a number");
  member.size = *size;
  return std::optional<TarMember>{std::move(member)};
}

std::array<std::uint8_t, kTarBlock> TarHeader(std::string_view name,
                                              std::uint64_t size) {
  std::array<std::uint8_t, kTarBlock> block{};
  std::ranges::copy(name.substr(0, 99), block.begin());
  PutOctal(std::span{block}.subspan(100, 8), 0644);
  PutOctal(std::span{block}.subspan(108, 8), 0);
  PutOctal(std::span{block}.subspan(116, 8), 0);
  PutOctal(std::span{block}.subspan(124, 12), size);
  PutOctal(std::span{block}.subspan(136, 12), 0);
  block[156] = '0';
  constexpr std::string_view kUstar{
      "ustar\0"
      "00",
      8};
  std::ranges::copy(kUstar, block.begin() + 257);
  const auto sum = Checksum(block);
  // The checksum field: six digits, NUL, space (as tar writes it).
  auto field = std::span{block}.subspan(148, 8);
  PutOctal(field.first(7), sum);
  field[7] = ' ';
  return block;
}

std::expected<void, std::string> TarReader::Header() {
  auto parsed = ParseTarHeader(block_);
  if (!parsed) return std::unexpected(parsed.error());
  const std::optional<TarMember>& next = *parsed;
  if (!next.has_value()) {
    if (members_ != kMembers.size()) {
      return std::unexpected("the package ends after " +
                             std::to_string(members_) + " of 3 files");
    }
    state_ = State::kEnd;
    return {};
  }
  const TarMember& member = *next;
  if (members_ >= kMembers.size()) {
    return std::unexpected("more than three files in the package ('" +
                           member.name + "')");
  }
  if (member.name != kMembers[members_]) {
    return std::unexpected("expected " + std::string{kMembers[members_]} +
                           ", found '" + member.name + "'");
  }
  if (member.size > Limit(member.name)) {
    return std::unexpected(member.name + " is too large (" +
                           std::to_string(member.size) + " bytes)");
  }
  ++members_;
  if (auto ok = sink_.Begin(member.name, member.size); !ok) return ok;
  left_ = member.size;
  if (left_ == 0) {
    state_ = State::kHeader;
    return sink_.End();
  }
  state_ = State::kData;
  // The padding after the data, to the next block.
  padding_ = (kTarBlock - (member.size % kTarBlock)) % kTarBlock;
  return {};
}

std::expected<void, std::string> TarReader::Feed(
    std::span<const std::uint8_t> data) {
  while (!data.empty()) {
    switch (state_) {
      case State::kHeader: {
        const auto n = std::min(data.size(), kTarBlock - block_fill_);
        std::ranges::copy(data.first(n), block_.begin() + block_fill_);
        block_fill_ += n;
        data = data.subspan(n);
        if (block_fill_ == kTarBlock) {
          block_fill_ = 0;
          if (auto ok = Header(); !ok) return ok;
        }
        break;
      }
      case State::kData: {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(data.size(), left_));
        if (auto ok = sink_.Write(data.first(n)); !ok) return ok;
        left_ -= n;
        data = data.subspan(n);
        if (left_ == 0) {
          if (auto ok = sink_.End(); !ok) return ok;
          left_ = padding_;
          state_ = left_ == 0 ? State::kHeader : State::kPadding;
        }
        break;
      }
      case State::kPadding: {
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(data.size(), left_));
        left_ -= n;
        data = data.subspan(n);
        if (left_ == 0) state_ = State::kHeader;
        break;
      }
      case State::kEnd:
        // Only zeros after the end (the second end block, record padding).
        if (!std::ranges::all_of(data, [](std::uint8_t b) { return b == 0; })) {
          return std::unexpected("data after the end of the tar");
        }
        data = {};
        break;
    }
  }
  return {};
}

std::expected<void, std::string> TarReader::Finish() {
  if (state_ != State::kEnd) {
    return std::unexpected(members_ < kMembers.size()
                               ? "the package ends after " +
                                     std::to_string(members_) + " of 3 files"
                               : std::string{"the tar has no end"});
  }
  return {};
}

}  // namespace aember::utils::package
