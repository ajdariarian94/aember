/**
 * @file install.cpp
 * @author Arian Ajdari
 * @brief Verifying, unpacking and installing .aember packages, rollback and
 *        recovery after a cut-off install.
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/install.h>
#include <aember-libs/utils/package/writer.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace pkg = aember::utils::package;

class TempDir {
 public:
  TempDir() {
    std::string tmpl =
        (fs::temp_directory_path() / "aember-package-XXXXXX").string();
    path_ = ::mkdtemp(tmpl.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::string Read(const std::string& path) {
  std::ifstream in{path, std::ios::binary};
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

void Write(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

pkg::KeyPair Key(std::uint8_t fill) {
  std::array<std::uint8_t, 32> seed{};
  seed.fill(fill);
  return pkg::KeyPairFromSeed(seed);
}

pkg::PublicKey Public(const pkg::KeyPair& pair, std::string name) {
  return *pkg::ParsePublicKey(pkg::PublicKeyText(pair.public_key),
                              std::move(name));
}

std::string Config(const std::string& name, const std::string& extra = "") {
  return R"({"name": ")" + name + R"(", "command": ["/app"])" + extra + "}";
}

/// A package's bytes: config, lxc, image, signed with @p key or not.
std::vector<std::uint8_t> Package(const std::string& config,
                                  const std::string& image,
                                  const std::optional<pkg::KeyPair>& key) {
  const auto payload =
      pkg::Compress(pkg::MakeTar(config, "lxc.uts.name = x\n", image));
  return pkg::Seal(*payload, key);
}

struct Fixture : ::testing::Test {
  TempDir tmp;
  std::string root = tmp.path() + "/lib";
  std::string inbox = tmp.path() + "/inbox";
  pkg::KeyPair key = Key(1);
  pkg::InstallOptions options;

  void SetUp() override {
    fs::create_directories(inbox);
    options.root = root;
    options.policy.keys = {Public(key, "dev")};
  }

  std::string Put(const std::string& name,
                  const std::vector<std::uint8_t>& bytes) {
    const auto path = inbox + "/" + name + ".aember";
    Write(path, bytes);
    return path;
  }
};

// ---------------------------------------------------------------------------
// Verify
// ---------------------------------------------------------------------------

using PackageVerifyTest = Fixture;

TEST_F(PackageVerifyTest, SignedByAKnownKey) {
  const auto path = Put("app", Package(Config("app"), "IMG", key));
  const auto v = pkg::Verify(path, options.policy);
  ASSERT_TRUE(v) << v.error();
  EXPECT_EQ(v->signed_by, "dev");
}

TEST_F(PackageVerifyTest, RejectsWhatIsntGenuine) {
  const auto good = Package(Config("app"), "IMG", key);
  // Unknown key.
  EXPECT_FALSE(
      pkg::Verify(Put("a", Package(Config("a"), "I", Key(2))), options.policy));
  // A byte of the payload changed: the digest doesn't match.
  auto tampered = good;
  tampered[3] ^= 1U;
  EXPECT_FALSE(pkg::Verify(Put("b", tampered), options.policy));
  // A changed digest: the signature doesn't match.
  auto resigned = good;
  resigned[resigned.size() - 160 + 20] ^= 1U;
  EXPECT_FALSE(pkg::Verify(Put("c", resigned), options.policy));
  // Data appended: the payload size doesn't match the file.
  auto longer = good;
  longer.insert(longer.end() - 160, 0);
  EXPECT_FALSE(pkg::Verify(Put("d", longer), options.policy));
  // Truncated, empty.
  EXPECT_FALSE(pkg::Verify(
      Put("e", std::vector<std::uint8_t>(good.begin(), good.end() - 1)),
      options.policy));
  EXPECT_FALSE(pkg::Verify(Put("f", {}), options.policy));
  EXPECT_FALSE(pkg::Verify(inbox + "/missing.aember", options.policy));
}

TEST_F(PackageVerifyTest, UnsignedOnlyWhenAllowed) {
  const auto path = Put("app", Package(Config("app"), "IMG", std::nullopt));
  EXPECT_FALSE(pkg::Verify(path, options.policy));
  options.policy.allow_unsigned = true;
  const auto v = pkg::Verify(path, options.policy);
  ASSERT_TRUE(v) << v.error();
  EXPECT_FALSE(v->signed_by);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

using PackageInstallTest = Fixture;

TEST_F(PackageInstallTest, UnpacksOnceAndRemovesThePackage) {
  const auto path = Put("app", Package(Config("app"), "IMAGE-1", key));
  const auto r = pkg::Install(path, options);
  ASSERT_TRUE(r) << r.error();
  EXPECT_TRUE(r->changed);
  EXPECT_EQ(r->name, "app");
  EXPECT_EQ(r->signed_by, "dev");
  EXPECT_FALSE(fs::exists(path));
  EXPECT_EQ(Read(root + "/app/image.squashfs"), "IMAGE-1");
  EXPECT_EQ(Read(root + "/app/lxc.container.conf"), "lxc.uts.name = x\n");
  EXPECT_EQ(Read(root + "/app/config.json"), Config("app"));
  const auto info = pkg::ReadInstalled(root + "/app");
  ASSERT_TRUE(info);
  EXPECT_EQ(info->digest, r->digest);
  EXPECT_EQ(info->signed_by, "dev");
  EXPECT_FALSE(fs::exists(root + "/app.prev"));
  EXPECT_FALSE(fs::exists(root + "/.app.new"));

  // The same package again: nothing changes, the file still goes.
  const auto again = Put("app", Package(Config("app"), "IMAGE-1", key));
  const auto r2 = pkg::Install(again, options);
  ASSERT_TRUE(r2) << r2.error();
  EXPECT_FALSE(r2->changed);
  EXPECT_FALSE(fs::exists(again));
  EXPECT_FALSE(fs::exists(root + "/app.prev"));
}

TEST_F(PackageInstallTest, ANewVersionKeepsThePreviousForARollback) {
  ASSERT_TRUE(
      pkg::Install(Put("app", Package(Config("app"), "V1", key)), options));
  ASSERT_TRUE(
      pkg::Install(Put("app", Package(Config("app"), "V2", key)), options));
  EXPECT_EQ(Read(root + "/app/image.squashfs"), "V2");
  EXPECT_EQ(Read(root + "/app.prev/image.squashfs"), "V1");

  ASSERT_TRUE(pkg::Rollback(root, "app"));
  EXPECT_EQ(Read(root + "/app/image.squashfs"), "V1");
  EXPECT_EQ(Read(root + "/app.prev/image.squashfs"), "V2");
  ASSERT_TRUE(pkg::Rollback(root, "app"));  // and back
  EXPECT_EQ(Read(root + "/app/image.squashfs"), "V2");

  EXPECT_FALSE(pkg::Rollback(root, "other"));
  EXPECT_FALSE(pkg::Rollback(root, "../etc"));
}

TEST_F(PackageInstallTest, ABadPackageChangesNothing) {
  ASSERT_TRUE(
      pkg::Install(Put("app", Package(Config("app"), "V1", key)), options));
  const auto check = [&](const std::string& what) {
    EXPECT_EQ(Read(root + "/app/image.squashfs"), "V1") << what;
    EXPECT_FALSE(fs::exists(root + "/.app.new")) << what;
    EXPECT_FALSE(fs::exists(root + "/app.prev")) << what;
  };

  // Its config.json names another package.
  EXPECT_FALSE(
      pkg::Install(Put("app", Package(Config("other"), "V2", key)), options));
  check("name");
  // config.json isn't JSON.
  EXPECT_FALSE(pkg::Install(Put("app", Package("{", "V2", key)), options));
  check("json");
  // The caller's check refuses it.
  auto strict = options;
  strict.check_config =
      [](const nlohmann::json&) -> std::expected<void, std::string> {
    return std::unexpected("no");
  };
  EXPECT_FALSE(
      pkg::Install(Put("app", Package(Config("app"), "V2", key)), strict));
  check("check_config");
  // The payload isn't xz.
  EXPECT_FALSE(pkg::Install(
      Put("app", pkg::Seal(pkg::MakeTar(Config("app"), "", "V2"), key)),
      options));
  check("not xz");
  // The tar isn't ours.
  EXPECT_FALSE(pkg::Install(
      Put("app",
          pkg::Seal(*pkg::Compress(std::vector<std::uint8_t>(2048, 0)), key)),
      options));
  check("empty tar");
  // Not signed.
  EXPECT_FALSE(pkg::Install(
      Put("app", Package(Config("app"), "V2", std::nullopt)), options));
  check("unsigned");
}

TEST_F(PackageInstallTest, FileNames) {
  const auto bytes = Package(Config("app"), "V1", key);
  const auto other = inbox + "/app.tar";
  Write(other, bytes);
  EXPECT_FALSE(pkg::Install(other, options));
  Write(inbox + "/Bad Name.aember", bytes);
  EXPECT_FALSE(pkg::Install(inbox + "/Bad Name.aember", options));
  EXPECT_FALSE(fs::exists(root + "/Bad Name"));
}

// The same format on both sides: a package aember's Python tools made
// (aember-cli/aember/utils/aember_package.py, signed with the RFC 8032 test
// key) verifies and installs here.
TEST_F(PackageInstallTest, APackageFromThePythonTools) {
  const auto fixture =
      std::string{AEMBER_PACKAGE_TEST_DATA} + "/python-made.aember";
  if (!fs::exists(fixture)) {
    GTEST_SKIP() << fixture << " isn't here (a test run on the device)";
  }
  const auto copy = inbox + "/python-made.aember";
  fs::copy_file(fixture, copy);
  std::array<std::uint8_t, 32> rfc_seed{};
  const std::string_view hex =
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
  for (std::size_t i = 0; i < rfc_seed.size(); ++i) {
    rfc_seed[i] = static_cast<std::uint8_t>(
        std::stoi(std::string{hex.substr(2 * i, 2)}, nullptr, 16));
  }
  options.policy.keys = {Public(pkg::KeyPairFromSeed(rfc_seed), "rfc")};
  const auto r = pkg::Install(copy, options);
  ASSERT_TRUE(r) << r.error();
  EXPECT_EQ(r->signed_by, "rfc");
  EXPECT_EQ(Read(root + "/python-made/lxc.container.conf"),
            "lxc.uts.name = python-made\n");
  EXPECT_EQ(Read(root + "/python-made/image.squashfs").size(), 4U + 1024U);
}

// ---------------------------------------------------------------------------
// Recover: what a power loss mid-install leaves
// ---------------------------------------------------------------------------

using PackageRecoverTest = Fixture;

TEST_F(PackageRecoverTest, AMissingVersionComesBack) {
  fs::create_directories(root);
  // Cut off between the renames (no RENAME_EXCHANGE): the complete new
  // version wins.
  fs::create_directories(root + "/.a.new");
  std::ofstream{root + "/.a.new/package.json"} << R"({"name":"a"})";
  fs::create_directories(root + "/a.prev");
  // Only a previous version: it comes back.
  fs::create_directories(root + "/b.prev");
  // An unpack that never finished, next to a good version: removed.
  fs::create_directories(root + "/c");
  fs::create_directories(root + "/.c.new");
  // An exchange cut off after its first rename.
  fs::create_directories(root + "/d.exchange");
  // And an exchange that left the old version behind.
  fs::create_directories(root + "/e");
  fs::create_directories(root + "/e.exchange");

  std::vector<std::string> notes;
  pkg::Recover(root, notes);
  EXPECT_TRUE(fs::exists(root + "/a/package.json"));
  EXPECT_TRUE(fs::exists(root + "/a.prev"));
  EXPECT_TRUE(fs::exists(root + "/b"));
  EXPECT_FALSE(fs::exists(root + "/b.prev"));
  EXPECT_TRUE(fs::exists(root + "/c"));
  EXPECT_FALSE(fs::exists(root + "/.c.new"));
  EXPECT_TRUE(fs::exists(root + "/d"));
  EXPECT_TRUE(fs::exists(root + "/e"));
  EXPECT_TRUE(fs::exists(root + "/e.prev"));
  EXPECT_FALSE(fs::exists(root + "/e.exchange"));
  EXPECT_FALSE(notes.empty());

  notes.clear();
  pkg::Recover(root, notes);  // nothing left to do
  EXPECT_TRUE(notes.empty());
  pkg::Recover(tmp.path() + "/none", notes);  // no root: nothing
}

TEST_F(PackageRecoverTest, ExchangeSwapsTwoDirectories) {
  fs::create_directories(root + "/x");
  fs::create_directories(root + "/y");
  std::ofstream{root + "/x/f"} << "x";
  std::ofstream{root + "/y/f"} << "y";
  ASSERT_TRUE(pkg::Exchange(root + "/x", root + "/y"));
  EXPECT_EQ(Read(root + "/x/f"), "y");
  EXPECT_EQ(Read(root + "/y/f"), "x");
  EXPECT_FALSE(pkg::Exchange(root + "/x", root + "/missing"));
}

}  // namespace
