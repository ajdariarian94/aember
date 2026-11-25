/**
 * @file package.cpp
 * @author Arian Ajdari
 * @brief The .aember format: footer, keys, signatures, the tar.
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/package.h>
#include <aember-libs/utils/package/tar.h>
#include <aember-libs/utils/package/writer.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <map>
#include <string>

#include <aember-test/property.h>

namespace {

namespace pkg = aember::utils::package;

std::array<std::uint8_t, 32> FromHex32(std::string_view hex) {
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint8_t>(
        std::stoi(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
  }
  return out;
}

/// Keeps what the tar holds.
class MemorySink final : public pkg::Sink {
 public:
  std::map<std::string, std::string> files;
  std::vector<std::string> order;

  std::expected<void, std::string> Begin(std::string_view name,
                                         std::uint64_t) override {
    current_ = std::string{name};
    order.push_back(current_);
    files[current_];
    return {};
  }
  std::expected<void, std::string> Write(
      std::span<const std::uint8_t> data) override {
    files[current_].append(data.begin(), data.end());
    return {};
  }
  std::expected<void, std::string> End() override { return {}; }

 private:
  std::string current_;
};

std::expected<void, std::string> ReadTar(const std::vector<std::uint8_t>& tar,
                                         MemorySink& sink) {
  pkg::TarReader reader{sink};
  if (auto ok = reader.Feed(tar); !ok) return ok;
  return reader.Finish();
}

// ---------------------------------------------------------------------------
// Footer, names, keys
// ---------------------------------------------------------------------------

TEST(PackageFooterTest, RoundTrip) {
  pkg::Footer f;
  f.payload_size = 0x0102030405060708ULL;
  f.digest.fill(7);
  f.key_id.fill(1);
  f.signature.fill(9);
  const auto bytes = pkg::SerializeFooter(f);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 8), "AEMBERv1");
  EXPECT_EQ(bytes[8], 0x08);  // little endian
  const auto back = pkg::ParseFooter(bytes);
  ASSERT_TRUE(back) << back.error();
  EXPECT_EQ(back->payload_size, f.payload_size);
  EXPECT_EQ(back->digest, f.digest);
  EXPECT_EQ(back->key_id, f.key_id);
  EXPECT_EQ(back->signature, f.signature);
  EXPECT_TRUE(back->Signed());
}

TEST(PackageFooterTest, Rejects) {
  pkg::Footer f;
  auto bytes = pkg::SerializeFooter(f);
  EXPECT_TRUE(pkg::ParseFooter(bytes));
  EXPECT_FALSE(pkg::ParseFooter(std::span{bytes}.first(100)));
  bytes[0] = 'X';
  EXPECT_FALSE(pkg::ParseFooter(bytes));
  bytes = pkg::SerializeFooter(f);
  bytes[159] = 'X';
  EXPECT_FALSE(pkg::ParseFooter(bytes));
  bytes = pkg::SerializeFooter(f);
  bytes[100] = 1;  // a signature but no key id
  EXPECT_FALSE(pkg::ParseFooter(bytes));
}

TEST(PackageNameTest, ValidNames) {
  for (const char* ok : {"aember-echo", "a", "web_1.2", "x0"}) {
    EXPECT_TRUE(pkg::ValidName(ok)) << ok;
  }
  for (const char* bad : {"",
                          "-x",
                          ".hidden",
                          "Upper",
                          "a/b",
                          "..",
                          "x.prev",
                          "x.new",
                          "containers",
                          "packages",
                          "with space"}) {
    EXPECT_FALSE(pkg::ValidName(bad)) << bad;
  }
  EXPECT_FALSE(pkg::ValidName(std::string(65, 'a')));
  EXPECT_TRUE(pkg::ValidName(std::string(64, 'a')));
}

// RFC 8032, 7.1, test 1: Monocypher's Ed25519 is the standard one (the
// Python tools sign with their own implementation).
TEST(PackageKeyTest, Rfc8032Vector) {
  const auto pair = pkg::KeyPairFromSeed(FromHex32(
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
  EXPECT_EQ(pkg::Hex(pair.public_key),
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
}

TEST(PackageKeyTest, TextRoundTripAndId) {
  const auto pair = pkg::KeyPairFromSeed(FromHex32(
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
  const auto text = pkg::PublicKeyText(pair.public_key);
  EXPECT_EQ(text, "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=\n");
  const auto key = pkg::ParsePublicKey(text, "dev");
  ASSERT_TRUE(key) << key.error();
  EXPECT_EQ(key->key, pair.public_key);
  EXPECT_EQ(key->id, pkg::KeyIdOf(pair.public_key));
  EXPECT_EQ(key->name, "dev");
  EXPECT_FALSE(pkg::ParsePublicKey("not base64!", "x"));
  EXPECT_FALSE(pkg::ParsePublicKey("AAAA", "x"));  // 3 bytes
  EXPECT_FALSE(pkg::ParsePublicKey("", "x"));
}

// ---------------------------------------------------------------------------
// Tar
// ---------------------------------------------------------------------------

TEST(PackageTarTest, HeaderRoundTrip) {
  const auto block = pkg::TarHeader("config.json", 12345);
  const auto member = pkg::ParseTarHeader(block);
  ASSERT_TRUE(member) << member.error();
  ASSERT_TRUE(*member);
  EXPECT_EQ((*member)->name, "config.json");
  EXPECT_EQ((*member)->size, 12345U);
  std::array<std::uint8_t, pkg::kTarBlock> zero{};
  const auto end = pkg::ParseTarHeader(zero);
  ASSERT_TRUE(end);
  EXPECT_FALSE(*end);
}

TEST(PackageTarTest, HeaderRejects) {
  auto block = pkg::TarHeader("config.json", 1);
  block[0] = 'C';  // checksum no longer matches
  EXPECT_FALSE(pkg::ParseTarHeader(block));
  const auto with = [](auto change) {
    auto b = pkg::TarHeader("x", 1);
    change(b);
    // Fix the checksum so only the change counts.
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
      sum += (i >= 148 && i < 156) ? ' ' : b[i];
    }
    const auto field = std::format("{:06o}", sum);
    std::copy(field.begin(), field.end(), b.begin() + 148);
    b[154] = 0;
    b[155] = ' ';
    return pkg::ParseTarHeader(b);
  };
  EXPECT_TRUE(with([](auto&) {}));
  EXPECT_FALSE(with([](auto& b) { b[156] = '5'; }));  // a directory
  EXPECT_FALSE(with([](auto& b) { b[156] = '2'; }));  // a symlink
  EXPECT_FALSE(with([](auto& b) { b[0] = '/'; }));    // not a plain name
  EXPECT_FALSE(with([](auto& b) { b[257] = 'X'; }));  // not ustar
  EXPECT_FALSE(with([](auto& b) { b[345] = 'p'; }));  // a prefix
  EXPECT_FALSE(with([](auto& b) { b[124] = '9'; }));  // size not octal
}

TEST(PackageTarTest, ReadsTheThreeFiles) {
  MemorySink sink;
  const auto tar = pkg::MakeTar("{}", "lxc.uts.name = x\n", "IMAGE");
  ASSERT_TRUE(ReadTar(tar, sink));
  EXPECT_EQ(sink.order,
            (std::vector<std::string>{
                "config.json", "lxc.container.conf", "image.squashfs"}));
  EXPECT_EQ(sink.files["image.squashfs"], "IMAGE");
  EXPECT_EQ(sink.files["lxc.container.conf"], "lxc.uts.name = x\n");
}

TEST(PackageTarProperty, AnySplitReadsTheSame) {
  aember::test::ForAll(200, [](aember::test::Gen& g) {
    const auto config = g.Bytes(700);
    const auto lxc = g.Bytes(600);
    const auto image = g.Bytes(3000);
    const auto tar = pkg::MakeTar(config, lxc, image);
    MemorySink sink;
    pkg::TarReader reader{sink};
    std::span<const std::uint8_t> rest{tar};
    while (!rest.empty()) {
      const auto n = std::min<std::size_t>(
          rest.size(), static_cast<std::size_t>(g.Int(1, 1500)));
      ASSERT_TRUE(reader.Feed(rest.first(n)));
      rest = rest.subspan(n);
    }
    ASSERT_TRUE(reader.Finish());
    EXPECT_EQ(sink.files["config.json"], config);
    EXPECT_EQ(sink.files["lxc.container.conf"], lxc);
    EXPECT_EQ(sink.files["image.squashfs"], image);
  });
}

TEST(PackageTarTest, OnlyTheThreeFilesInOrder) {
  const auto tar_of = [](std::vector<std::pair<std::string, std::string>> files,
                         int end_blocks = 2) {
    std::vector<std::uint8_t> tar;
    for (const auto& [name, data] : files) {
      const auto h = pkg::TarHeader(name, data.size());
      tar.insert(tar.end(), h.begin(), h.end());
      tar.insert(tar.end(), data.begin(), data.end());
      tar.resize(tar.size() + ((512 - (data.size() % 512)) % 512));
    }
    tar.resize(tar.size() + (512 * static_cast<std::size_t>(end_blocks)));
    return tar;
  };
  MemorySink sink;
  EXPECT_TRUE(ReadTar(tar_of({{"config.json", "{}"},
                              {"lxc.container.conf", ""},
                              {"image.squashfs", "x"}}),
                      sink));
  MemorySink s1;
  EXPECT_FALSE(ReadTar(tar_of({{"lxc.container.conf", ""},
                               {"config.json", "{}"},
                               {"image.squashfs", "x"}}),
                       s1));
  MemorySink s2;
  EXPECT_FALSE(
      ReadTar(tar_of({{"config.json", "{}"}, {"lxc.container.conf", ""}}), s2));
  MemorySink s3;
  EXPECT_FALSE(ReadTar(tar_of({{"config.json", "{}"},
                               {"lxc.container.conf", ""},
                               {"image.squashfs", "x"},
                               {"extra", "y"}}),
                       s3));
  MemorySink s4;
  EXPECT_FALSE(ReadTar(tar_of({{"config.json", std::string(70000, 'x')},
                               {"lxc.container.conf", ""},
                               {"image.squashfs", "x"}}),
                       s4))
      << "config.json over 64 KiB";
  MemorySink s5;
  EXPECT_FALSE(ReadTar(tar_of({{"config.json", "{}"},
                               {"lxc.container.conf", ""},
                               {"image.squashfs", "x"}},
                              0),
                       s5))
      << "no end blocks";
  MemorySink s6;
  auto junk = tar_of({{"config.json", "{}"},
                      {"lxc.container.conf", ""},
                      {"image.squashfs", "x"}});
  junk.push_back(1);
  EXPECT_FALSE(ReadTar(junk, s6)) << "data after the end";
}

}  // namespace
