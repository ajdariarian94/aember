/**
 * @file install.h
 * @author Arian Ajdari
 * @brief Unpacking `.aember` packages onto the device's disk, once, and
 *        safe against a power loss at any moment:
 *
 *   <root>/<name>/            config.json, lxc.container.conf,
 *                             image.squashfs, package.json (what's in)
 *   <root>/<name>.prev/       the version before (for a rollback)
 *   <root>/.<name>.new/       an unpack in progress
 *
 *   Install: verify, unpack into .<name>.new (every file and the directory
 *   synced, package.json last), swap it with <name> in one step
 *   (renameat2 RENAME_EXCHANGE), keep the old one as <name>.prev, remove
 *   the package file. <name> never goes missing, and an interrupted
 *   install leaves either the old or the new version in place.
 *
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#pragma once

#include <aember-libs/utils/package/package.h>
#include <aember-libs/utils/package/tar.h>

#include <nlohmann/json.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aember::utils::package {

/// Decode @p path's payload (one xz stream of exactly footer.payload_size
/// bytes) into @p sink through the tar reader. Run Verify() first: this
/// trusts the footer.
std::expected<void, std::string> Unpack(const std::string& path,
                                        const Footer& footer, Sink& sink);
/// The same from an open file (read with pread; the offset isn't used).
std::expected<void, std::string> Unpack(int fd, const Footer& footer,
                                        Sink& sink);

/// Writes the members into a directory (files created new, synced when
/// complete). Refuses an image larger than the file system's free space.
class DirectorySink final : public Sink {
 public:
  explicit DirectorySink(std::string dir) : dir_(std::move(dir)) {}
  ~DirectorySink() override;
  DirectorySink(const DirectorySink&) = delete;
  DirectorySink& operator=(const DirectorySink&) = delete;

  std::expected<void, std::string> Begin(std::string_view name,
                                         std::uint64_t size) override;
  std::expected<void, std::string> Write(
      std::span<const std::uint8_t> data) override;
  std::expected<void, std::string> End() override;

 private:
  std::string dir_;
  std::string name_;
  int fd_{-1};
};

/// What package.json of an unpacked package says.
struct InstalledInfo {
  std::string name;
  std::string digest;  ///< hex SHA-512 of the payload
  std::optional<std::string> signed_by;
};

std::optional<InstalledInfo> ReadInstalled(const std::string& dir);

struct InstallOptions {
  std::string root = "/var/lib/aember";
  Policy policy;
  /// Checks the package's config.json before it replaces anything (e.g.
  /// containerd's parser): an error stops the install.
  std::function<std::expected<void, std::string>(const nlohmann::json&)>
      check_config;
};

struct InstallResult {
  std::string name;
  bool changed{false};  ///< false: the same package was in place already
  std::string digest;
  std::optional<std::string> signed_by;
};

/// Install @p package (named <name>.aember) under options.root, then remove
/// the package file. Installing what is already there changes nothing (and
/// still removes the file).
std::expected<InstallResult, std::string> Install(
    const std::string& package, const InstallOptions& options);

/// Swap <name> and <name>.prev (a second rollback swaps them back).
std::expected<void, std::string> Rollback(const std::string& root,
                                          std::string_view name);

/// Clean up after an install that was cut off: a <name> that is missing
/// comes back (the complete new version, else the previous one), and
/// leftover .<name>.new directories go. What it did goes into @p notes.
void Recover(const std::string& root, std::vector<std::string>& notes);

/// Swap two paths in one step (renameat2 RENAME_EXCHANGE); where the file
/// system can't, three renames through a temporary name.
std::expected<void, std::string> Exchange(const std::string& a,
                                          const std::string& b);

/// fsync a directory (makes renames in it durable).
std::expected<void, std::string> SyncDirectory(const std::string& dir);

}  // namespace aember::utils::package
