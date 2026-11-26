/**
 * @file writer.h
 * @author Arian Ajdari
 * @brief Writing `.aember` packages (tests, fuzz seeds). The tools that
 *        make real packages are in Python: aember-build-helper's
 *        `aember build` and aember's `aember package`; this writes the
 *        same bytes.
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/package/package.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aember::utils::package {

/// An Ed25519 key pair from a 32-byte seed.
struct KeyPair {
  std::array<std::uint8_t, 32> seed{};
  PublicKeyBytes public_key{};
};
KeyPair KeyPairFromSeed(const std::array<std::uint8_t, 32>& seed);
/// The public key as a .pub file holds it (base64 and a newline).
std::string PublicKeyText(const PublicKeyBytes& key);

/// A tar of the three members (the payload before compression).
std::vector<std::uint8_t> MakeTar(std::string_view config, std::string_view lxc,
                                  std::string_view image);
/// xz (preset 6, CRC64) of @p data as one stream.
std::expected<std::vector<std::uint8_t>, std::string> Compress(
    const std::vector<std::uint8_t>& data);
/// A whole package from a (compressed) payload: footer appended, signed
/// with @p key unless nullopt.
std::vector<std::uint8_t> Seal(const std::vector<std::uint8_t>& payload,
                               const std::optional<KeyPair>& key);

}  // namespace aember::utils::package
