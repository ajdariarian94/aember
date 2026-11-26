/**
 * @file writer.cpp
 * @author Arian Ajdari
 * @brief Writing `.aember` packages (see writer.h).
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/tar.h>
#include <aember-libs/utils/package/writer.h>

#include <algorithm>

#include <lzma.h>
#include <monocypher.h>

#include <monocypher-ed25519.h>

namespace aember::utils::package {

KeyPair KeyPairFromSeed(const std::array<std::uint8_t, 32>& seed) {
  KeyPair pair{.seed = seed, .public_key = {}};
  auto scratch = seed;  // key_pair wipes the seed it gets
  std::array<std::uint8_t, 64> secret{};
  crypto_ed25519_key_pair(
      secret.data(), pair.public_key.data(), scratch.data());
  crypto_wipe(secret.data(), secret.size());
  return pair;
}

std::string PublicKeyText(const PublicKeyBytes& key) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 3 <= key.size(); i += 3) {
    const unsigned n = (static_cast<unsigned>(key[i]) << 16U) |
                       (static_cast<unsigned>(key[i + 1]) << 8U) | key[i + 2];
    for (int shift = 18; shift >= 0; shift -= 6) {
      out.push_back(kAlphabet[(n >> static_cast<unsigned>(shift)) & 63U]);
    }
  }
  // 32 bytes: two left over.
  const unsigned n = (static_cast<unsigned>(key[i]) << 16U) |
                     (static_cast<unsigned>(key[i + 1]) << 8U);
  out.push_back(kAlphabet[(n >> 18U) & 63U]);
  out.push_back(kAlphabet[(n >> 12U) & 63U]);
  out.push_back(kAlphabet[(n >> 6U) & 63U]);
  out += "=\n";
  return out;
}

std::vector<std::uint8_t> MakeTar(std::string_view config, std::string_view lxc,
                                  std::string_view image) {
  std::vector<std::uint8_t> tar;
  const auto add = [&](std::string_view name, std::string_view data) {
    const auto header = TarHeader(name, data.size());
    tar.insert(tar.end(), header.begin(), header.end());
    tar.insert(tar.end(), data.begin(), data.end());
    tar.resize(tar.size() +
               ((kTarBlock - (data.size() % kTarBlock)) % kTarBlock));
  };
  add(kConfigFile, config);
  add(kLxcFile, lxc);
  add(kImageFile, image);
  tar.resize(tar.size() + (2 * kTarBlock));  // the end
  return tar;
}

std::expected<std::vector<std::uint8_t>, std::string> Compress(
    const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> out(lzma_stream_buffer_bound(data.size()));
  std::size_t pos = 0;
  if (lzma_easy_buffer_encode(6,
                              LZMA_CHECK_CRC64,
                              nullptr,
                              data.data(),
                              data.size(),
                              out.data(),
                              &pos,
                              out.size()) != LZMA_OK) {
    return std::unexpected("xz compression failed");
  }
  out.resize(pos);
  return out;
}

std::vector<std::uint8_t> Seal(const std::vector<std::uint8_t>& payload,
                               const std::optional<KeyPair>& key) {
  Footer footer;
  footer.payload_size = payload.size();
  crypto_sha512(footer.digest.data(), payload.data(), payload.size());
  if (key) {
    footer.key_id = KeyIdOf(key->public_key);
    auto seed = key->seed;
    std::array<std::uint8_t, 64> secret{};
    PublicKeyBytes pub{};
    crypto_ed25519_key_pair(secret.data(), pub.data(), seed.data());
    const auto message = SignedMessage(footer);
    crypto_ed25519_sign(
        footer.signature.data(), secret.data(), message.data(), message.size());
    crypto_wipe(secret.data(), secret.size());
  }
  auto out = payload;
  const auto bytes = SerializeFooter(footer);
  out.insert(out.end(), bytes.begin(), bytes.end());
  return out;
}

}  // namespace aember::utils::package
