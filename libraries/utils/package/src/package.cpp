/**
 * @file package.cpp
 * @author Arian Ajdari
 * @brief The `.aember` footer, keys and verification (see package.h).
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/package.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <monocypher-ed25519.h>

namespace aember::utils::package {

namespace {

constexpr std::size_t kSignedPart = 88;  // footer bytes the signature covers

void PutU64(std::uint8_t* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>(v >> (8 * i));  // NOLINT
  }
}

std::uint64_t GetU64(const std::uint8_t* in) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | in[i];  // NOLINT
  return v;
}

bool AllZero(std::span<const std::uint8_t> bytes) {
  return std::ranges::all_of(bytes, [](std::uint8_t b) { return b == 0; });
}

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/// Standard base64 with padding; nullopt for anything else.
std::optional<std::vector<std::uint8_t>> DecodeBase64(std::string_view text) {
  if (text.size() % 4 != 0) return std::nullopt;
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = text[i + static_cast<std::size_t>(j)];
      if (c == '=' && i + 4 == text.size() && j >= 2) {
        v[j] = 0;
        ++pad;
        continue;
      }
      if (pad > 0) return std::nullopt;  // data after '='
      v[j] = Base64Value(c);
      if (v[j] < 0) return std::nullopt;
    }
    const unsigned n = (static_cast<unsigned>(v[0]) << 18U) |
                       (static_cast<unsigned>(v[1]) << 12U) |
                       (static_cast<unsigned>(v[2]) << 6U) |
                       static_cast<unsigned>(v[3]);
    out.push_back(static_cast<std::uint8_t>(n >> 16U));
    if (pad < 2) out.push_back(static_cast<std::uint8_t>(n >> 8U));
    if (pad < 1) out.push_back(static_cast<std::uint8_t>(n));
  }
  return out;
}

class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  [[nodiscard]] int get() const { return fd_; }

 private:
  int fd_;
};

std::string Errno(std::string_view what) {
  return std::string{what} + ": " + std::system_category().message(errno);
}

/// pread all of @p out at @p offset.
bool ReadAt(int fd, std::span<std::uint8_t> out, std::uint64_t offset) {
  std::size_t done = 0;
  while (done < out.size()) {
    const auto n = ::pread(fd,
                           out.data() + done,
                           out.size() - done,
                           static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

bool Footer::Signed() const {
  return !AllZero(key_id);
}

std::expected<Footer, std::string> ParseFooter(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kFooterSize) {
    return std::unexpected("footer is not " + std::to_string(kFooterSize) +
                           " bytes");
  }
  const auto text = [&](std::size_t at) {
    return std::string_view{reinterpret_cast<const char*>(bytes.data() + at),
                            8};
  };
  if (text(0) != kMagic || text(152) != kEndMagic) {
    return std::unexpected("not an .aember package (no AEMBERv1 footer)");
  }
  Footer f;
  f.payload_size = GetU64(bytes.data() + 8);
  std::ranges::copy(bytes.subspan(16, 64), f.digest.begin());
  std::ranges::copy(bytes.subspan(80, 8), f.key_id.begin());
  std::ranges::copy(bytes.subspan(88, 64), f.signature.begin());
  if (!f.Signed() && !AllZero(f.signature)) {
    return std::unexpected("a signature without a key id");
  }
  return f;
}

std::array<std::uint8_t, kFooterSize> SerializeFooter(const Footer& footer) {
  std::array<std::uint8_t, kFooterSize> out{};
  std::ranges::copy(kMagic, out.begin());
  PutU64(out.data() + 8, footer.payload_size);
  std::ranges::copy(footer.digest, out.begin() + 16);
  std::ranges::copy(footer.key_id, out.begin() + 80);
  std::ranges::copy(footer.signature, out.begin() + 88);
  std::ranges::copy(kEndMagic, out.begin() + 152);
  return out;
}

std::vector<std::uint8_t> SignedMessage(const Footer& footer) {
  const auto bytes = SerializeFooter(footer);
  std::vector<std::uint8_t> message(kSignatureContext.begin(),
                                    kSignatureContext.end());
  message.push_back(0);
  message.insert(message.end(), bytes.begin(), bytes.begin() + kSignedPart);
  return message;
}

bool ValidName(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  const auto lower_or_digit = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  if (!lower_or_digit(name.front())) return false;
  if (!std::ranges::all_of(name, [&](char c) {
        return lower_or_digit(c) || c == '.' || c == '_' || c == '-';
      })) {
    return false;
  }
  if (name.ends_with(".prev") || name.ends_with(".new")) return false;
  return name != "containers" && name != "packages";
}

std::string Hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto b : bytes) {
    out.push_back(kDigits[b >> 4U]);
    out.push_back(kDigits[b & 0xfU]);
  }
  return out;
}

KeyId KeyIdOf(const PublicKeyBytes& key) {
  Digest hash{};
  crypto_sha512(hash.data(), key.data(), key.size());
  KeyId id{};
  std::ranges::copy(std::span{hash}.first(id.size()), id.begin());
  return id;
}

std::expected<PublicKey, std::string> ParsePublicKey(std::string_view text,
                                                     std::string name) {
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  const auto bytes = DecodeBase64(text);
  if (!bytes || bytes->size() != 32) {
    return std::unexpected("not a base64 Ed25519 public key (32 bytes)");
  }
  PublicKey key;
  key.name = std::move(name);
  std::ranges::copy(*bytes, key.key.begin());
  key.id = KeyIdOf(key.key);
  return key;
}

std::vector<PublicKey> LoadKeys(const std::string& dir,
                                std::vector<std::string>& errors) {
  std::vector<PublicKey> keys;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return keys;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
    if (entry.path().extension() == ".pub") files.push_back(entry.path());
  }
  std::ranges::sort(files);
  for (const auto& path : files) {
    std::ifstream in{path};
    std::stringstream text;
    text << in.rdbuf();
    if (text.str().size() > 1024) {
      errors.push_back(path.string() + ": too large for a key");
      continue;
    }
    auto key = ParsePublicKey(text.str(), path.stem().string());
    if (!key) {
      errors.push_back(path.string() + ": " + key.error());
      continue;
    }
    keys.push_back(std::move(*key));
  }
  return keys;
}

bool CheckSignature(const Footer& footer, const PublicKeyBytes& key) {
  const auto message = SignedMessage(footer);
  return crypto_ed25519_check(footer.signature.data(),
                              key.data(),
                              message.data(),
                              message.size()) == 0;
}

std::expected<Verified, std::string> Verify(const std::string& path,
                                            const Policy& policy) {
  const Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (fd.get() < 0) return std::unexpected(Errno(path));
  return Verify(fd.get(), path, policy);
}

std::expected<Verified, std::string> Verify(int fd, const std::string& path,
                                            const Policy& policy) {
  struct stat st {};
  if (::fstat(fd, &st) != 0) return std::unexpected(Errno(path));
  const auto size = static_cast<std::uint64_t>(st.st_size);
  if (!S_ISREG(st.st_mode) || size < kFooterSize) {
    return std::unexpected(path + ": too small for an .aember package");
  }
  std::array<std::uint8_t, kFooterSize> raw{};
  if (!ReadAt(fd, raw, size - kFooterSize)) {
    return std::unexpected(Errno(path));
  }
  auto footer = ParseFooter(raw);
  if (!footer) return std::unexpected(path + ": " + footer.error());
  if (footer->payload_size != size - kFooterSize) {
    return std::unexpected(path + ": the payload size doesn't match the file");
  }

  // The signature first (cheap): a package from an unknown key isn't even
  // hashed.
  Verified result{.footer = *footer, .signed_by = std::nullopt};
  if (footer->Signed()) {
    const auto key = std::ranges::find_if(policy.keys, [&](const PublicKey& k) {
      return k.id == footer->key_id;
    });
    if (key == policy.keys.end()) {
      return std::unexpected(path + ": signed by an unknown key (" +
                             Hex(footer->key_id) + ")");
    }
    if (!CheckSignature(*footer, key->key)) {
      return std::unexpected(path + ": bad signature");
    }
    result.signed_by = key->name;
  } else if (!policy.allow_unsigned) {
    return std::unexpected(path + ": not signed (only signed packages)");
  }

  crypto_sha512_ctx ctx;
  crypto_sha512_init(&ctx);
  std::vector<std::uint8_t> buffer(std::size_t{1} << 16);
  std::uint64_t offset = 0;
  while (offset < footer->payload_size) {
    const auto want = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), footer->payload_size - offset));
    if (!ReadAt(fd, std::span{buffer}.first(want), offset)) {
      return std::unexpected(Errno(path));
    }
    crypto_sha512_update(&ctx, buffer.data(), want);
    offset += want;
  }
  Digest digest{};
  crypto_sha512_final(&ctx, digest.data());
  if (digest != footer->digest) {
    return std::unexpected(path + ": damaged (SHA-512 doesn't match)");
  }
  return result;
}

}  // namespace aember::utils::package
