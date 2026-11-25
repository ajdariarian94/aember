/**
 * @file package.h
 * @author Arian Ajdari
 * @brief The `.aember` package format: one container (its SquashFS image,
 *        config.json and lxc.container.conf) in one compressed, signed
 *        file. The format is specified in doc/aember-format.md.
 *
 *   <payload: one xz stream of a ustar tar>  <footer: 160 bytes>
 *
 *   footer  0  8  "AEMBERv1"
 *           8  8  payload size (little endian)
 *          16 64  SHA-512 of the payload
 *          80  8  key id (first 8 bytes of SHA-512 of the public key);
 *                 zero: unsigned
 *          88 64  Ed25519 signature of "aember-package-v1\0" + footer[0:88];
 *                 zero: unsigned
 *         152  8  "1vREBMEA"
 *
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
#include <vector>

namespace aember::utils::package {

inline constexpr std::size_t kFooterSize = 160;
inline constexpr std::string_view kMagic = "AEMBERv1";
inline constexpr std::string_view kEndMagic = "1vREBMEA";
/// Signed with a trailing '\0': the message is this, a zero byte and the
/// first 88 bytes of the footer.
inline constexpr std::string_view kSignatureContext = "aember-package-v1";

/// The members of the tar, in this order, and nothing else.
inline constexpr std::string_view kConfigFile = "config.json";
inline constexpr std::string_view kLxcFile = "lxc.container.conf";
inline constexpr std::string_view kImageFile = "image.squashfs";
/// Written last into an unpacked package: what was installed (digest).
inline constexpr std::string_view kInstalledFile = "package.json";
inline constexpr std::string_view kExtension = ".aember";

/// Largest config.json / lxc.container.conf accepted.
inline constexpr std::uint64_t kMaxTextSize = 64 * 1024;
/// Largest image accepted (sanity bound, not a quota).
inline constexpr std::uint64_t kMaxImageSize = std::uint64_t{8} << 30;
/// Memory the xz decoder may use (packages are written with preset 6:
/// an 8 MiB dictionary, about 9 MiB to decode).
inline constexpr std::uint64_t kDecoderMemoryLimit = std::uint64_t{128} << 20;

using Digest = std::array<std::uint8_t, 64>;
using KeyId = std::array<std::uint8_t, 8>;
using Signature = std::array<std::uint8_t, 64>;
using PublicKeyBytes = std::array<std::uint8_t, 32>;

struct Footer {
  std::uint64_t payload_size{0};
  Digest digest{};
  KeyId key_id{};
  Signature signature{};

  /// A key id other than zero: the package claims a signature.
  [[nodiscard]] bool Signed() const;
};

/// The footer from the last kFooterSize bytes of a package. Never throws.
std::expected<Footer, std::string> ParseFooter(
    std::span<const std::uint8_t> bytes);
std::array<std::uint8_t, kFooterSize> SerializeFooter(const Footer& footer);
/// What the signature covers: kSignatureContext, '\0', footer[0:88].
std::vector<std::uint8_t> SignedMessage(const Footer& footer);

/// A name a package (and its directory on the device) may have:
/// [a-z0-9][a-z0-9._-]*, at most 64 characters, not ending in ".prev" or
/// ".new", not "containers" or "packages" (directories of their own there).
bool ValidName(std::string_view name);

/// Lower-case hex.
std::string Hex(std::span<const std::uint8_t> bytes);

struct PublicKey {
  std::string name;  ///< the key file's name without ".pub"
  PublicKeyBytes key{};
  KeyId id{};
};

KeyId KeyIdOf(const PublicKeyBytes& key);
/// A public key from its text form: the 32 bytes in base64 (one line; a
/// trailing newline is fine).
std::expected<PublicKey, std::string> ParsePublicKey(std::string_view text,
                                                     std::string name);
/// Every <dir>/<name>.pub. A missing directory is no keys; a file that
/// isn't a key is described in @p errors and left out.
std::vector<PublicKey> LoadKeys(const std::string& dir,
                                std::vector<std::string>& errors);

/// Which packages are accepted.
struct Policy {
  std::vector<PublicKey> keys;
  /// Accept packages without a signature. For development boards only.
  bool allow_unsigned{false};
};

/// Ed25519 verification of a footer's signature with @p key.
bool CheckSignature(const Footer& footer, const PublicKeyBytes& key);

struct Verified {
  Footer footer;
  /// The key that signed it; nullopt: unsigned (and the policy allows it).
  std::optional<std::string> signed_by;
};

/// Reads @p path once: its footer, the SHA-512 of its payload, the
/// signature against @p policy. Never throws.
std::expected<Verified, std::string> Verify(const std::string& path,
                                            const Policy& policy);
/// The same for an open file (@p what names it in errors): Install verifies
/// and unpacks the same open file, so it can't change in between.
std::expected<Verified, std::string> Verify(int fd, const std::string& what,
                                            const Policy& policy);

}  // namespace aember::utils::package
