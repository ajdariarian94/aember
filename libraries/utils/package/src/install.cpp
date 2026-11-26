/**
 * @file install.cpp
 * @author Arian Ajdari
 * @brief Unpacking and installing `.aember` packages (see install.h).
 * @version 0.1
 * @date 2026-11-26
 *
 * @copyright Copyright (c) 2026, Aember, All rights reserved.
 */

#include <aember-libs/utils/package/install.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <lzma.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace aember::utils::package {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kRenameExchange = 2;  // RENAME_EXCHANGE, linux/fs.h

std::string Errno(std::string_view what, int err = errno) {
  return std::string{what} + ": " + std::system_category().message(err);
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

bool Exists(const std::string& path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

void RemoveAll(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

/// Write @p text to @p path (new file) and fsync it.
std::expected<void, std::string> WriteSynced(const std::string& path,
                                             std::string_view text) {
  const Fd fd{
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
  if (fd.get() < 0) return std::unexpected(Errno(path));
  std::size_t done = 0;
  while (done < text.size()) {
    const auto n = ::write(fd.get(), text.data() + done, text.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return std::unexpected(Errno(path));
    done += static_cast<std::size_t>(n);
  }
  if (::fsync(fd.get()) != 0) return std::unexpected(Errno(path));
  return {};
}

std::optional<std::string> ReadText(const std::string& path,
                                    std::uint64_t limit) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > limit) return std::nullopt;
  std::ifstream in{path, std::ios::binary};
  if (!in) return std::nullopt;
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Unpack
// ---------------------------------------------------------------------------

std::expected<void, std::string> Unpack(const std::string& path,
                                        const Footer& footer, Sink& sink) {
  const Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
  if (fd.get() < 0) return std::unexpected(Errno(path));
  return Unpack(fd.get(), footer, sink);
}

std::expected<void, std::string> Unpack(int fd, const Footer& footer,
                                        Sink& sink) {
  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_stream_decoder(&strm, kDecoderMemoryLimit, 0) != LZMA_OK) {
    return std::unexpected("cannot start the xz decoder");
  }
  struct End {
    lzma_stream* s;
    ~End() { lzma_end(s); }
  } end{&strm};

  TarReader tar{sink};
  std::vector<std::uint8_t> in(std::size_t{1} << 16);
  std::vector<std::uint8_t> out(std::size_t{1} << 16);
  std::uint64_t offset = 0;
  bool stream_end = false;
  while (!stream_end) {
    if (strm.avail_in == 0 && offset < footer.payload_size) {
      const auto want = static_cast<std::size_t>(
          std::min<std::uint64_t>(in.size(), footer.payload_size - offset));
      const auto n = ::pread(fd, in.data(), want, static_cast<off_t>(offset));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return std::unexpected(Errno("reading the package"));
      offset += static_cast<std::uint64_t>(n);
      strm.next_in = in.data();
      strm.avail_in = static_cast<std::size_t>(n);
    }
    const auto action = offset == footer.payload_size ? LZMA_FINISH : LZMA_RUN;
    strm.next_out = out.data();
    strm.avail_out = out.size();
    const auto ret = lzma_code(&strm, action);
    const auto produced = out.size() - strm.avail_out;
    if (produced > 0) {
      if (auto ok = tar.Feed(std::span{out}.first(produced)); !ok) return ok;
    }
    if (ret == LZMA_STREAM_END) {
      stream_end = true;
    } else if (ret == LZMA_MEMLIMIT_ERROR) {
      return std::unexpected("the payload needs too much memory to unpack");
    } else if (ret != LZMA_OK) {
      return std::unexpected("the payload is not a valid xz stream (" +
                             std::to_string(static_cast<int>(ret)) + ")");
    }
  }
  if (strm.avail_in != 0 || offset != footer.payload_size) {
    return std::unexpected("data after the payload's xz stream");
  }
  return tar.Finish();
}

// ---------------------------------------------------------------------------
// DirectorySink
// ---------------------------------------------------------------------------

DirectorySink::~DirectorySink() {
  if (fd_ >= 0) ::close(fd_);
}

std::expected<void, std::string> DirectorySink::Begin(std::string_view name,
                                                      std::uint64_t size) {
  if (fd_ >= 0) return std::unexpected("a file is still open");
  name_ = dir_ + "/" + std::string{name};
  struct statvfs vfs {};
  if (::statvfs(dir_.c_str(), &vfs) == 0) {
    const auto free = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    if (size > free) {
      return std::unexpected("not enough space for " + std::string{name} +
                             " (" + std::to_string(size) + " bytes, " +
                             std::to_string(free) + " free)");
    }
  }
  fd_ = ::open(name_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd_ < 0) return std::unexpected(Errno(name_));
  return {};
}

std::expected<void, std::string> DirectorySink::Write(
    std::span<const std::uint8_t> data) {
  while (!data.empty()) {
    const auto n = ::write(fd_, data.data(), data.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return std::unexpected(Errno(name_));
    data = data.subspan(static_cast<std::size_t>(n));
  }
  return {};
}

std::expected<void, std::string> DirectorySink::End() {
  const bool synced = ::fsync(fd_) == 0;
  const int err = errno;
  ::close(fd_);
  fd_ = -1;
  if (!synced) return std::unexpected(Errno(name_, err));
  return {};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::expected<void, std::string> SyncDirectory(const std::string& dir) {
  const Fd fd{::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (fd.get() < 0) return std::unexpected(Errno(dir));
  if (::fsync(fd.get()) != 0) return std::unexpected(Errno(dir));
  return {};
}

std::expected<void, std::string> Exchange(const std::string& a,
                                          const std::string& b) {
  if (::syscall(SYS_renameat2,
                AT_FDCWD,
                a.c_str(),
                AT_FDCWD,
                b.c_str(),
                kRenameExchange) == 0) {
    return {};
  }
  if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) {
    return std::unexpected(Errno("exchange " + a + " and " + b));
  }
  // No RENAME_EXCHANGE here: three renames. A cut-off leaves b missing for
  // a moment, which Recover() repairs.
  const std::string tmp = b + ".exchange";
  if (std::rename(b.c_str(), tmp.c_str()) != 0) {
    return std::unexpected(Errno("rename " + b));
  }
  if (std::rename(a.c_str(), b.c_str()) != 0) {
    const int err = errno;
    std::ignore = std::rename(tmp.c_str(), b.c_str());
    return std::unexpected(Errno("rename " + a, err));
  }
  if (std::rename(tmp.c_str(), a.c_str()) != 0) {
    return std::unexpected(Errno("rename " + tmp));
  }
  return {};
}

std::optional<InstalledInfo> ReadInstalled(const std::string& dir) {
  const auto text =
      ReadText(dir + "/" + std::string{kInstalledFile}, kMaxTextSize);
  if (!text) return std::nullopt;
  const auto json = nlohmann::json::parse(*text, nullptr, false);
  if (!json.is_object()) return std::nullopt;
  const auto name = json.find("name");
  const auto digest = json.find("digest");
  if (name == json.end() || !name->is_string() || digest == json.end() ||
      !digest->is_string()) {
    return std::nullopt;
  }
  InstalledInfo info{.name = name->get<std::string>(),
                     .digest = digest->get<std::string>(),
                     .signed_by = std::nullopt};
  if (const auto key = json.find("signed_by");
      key != json.end() && key->is_string()) {
    info.signed_by = key->get<std::string>();
  }
  return info;
}

// ---------------------------------------------------------------------------
// Install / Rollback / Recover
// ---------------------------------------------------------------------------

std::expected<InstallResult, std::string> Install(
    const std::string& package, const InstallOptions& options) {
  const fs::path path{package};
  const auto file = path.filename().string();
  if (!file.ends_with(kExtension)) {
    return std::unexpected(package + ": not a " + std::string{kExtension} +
                           " file");
  }
  const auto name = file.substr(0, file.size() - kExtension.size());
  if (!ValidName(name)) {
    return std::unexpected(package + ": '" + name +
                           "' is not a valid package name");
  }
  const Fd fd{::open(package.c_str(), O_RDONLY | O_CLOEXEC)};
  if (fd.get() < 0) return std::unexpected(Errno(package));
  auto verified = Verify(fd.get(), package, options.policy);
  if (!verified) return std::unexpected(verified.error());

  InstallResult result{.name = name,
                       .changed = false,
                       .digest = Hex(verified->footer.digest),
                       .signed_by = verified->signed_by};
  const auto inbox = path.parent_path().string();
  const auto remove_package = [&] {
    if (::unlink(package.c_str()) != 0 && errno != ENOENT) {
      return std::expected<void, std::string>{
          std::unexpected(Errno("remove " + package))};
    }
    return SyncDirectory(inbox.empty() ? "." : inbox);
  };

  const std::string root = options.root;
  const std::string dest = root + "/" + name;
  if (const auto installed = ReadInstalled(dest);
      installed && installed->digest == result.digest) {
    if (auto ok = remove_package(); !ok) return std::unexpected(ok.error());
    return result;  // the same package is in place already
  }

  std::error_code ec;
  fs::create_directories(root, ec);
  const std::string tmp = root + "/." + name + ".new";
  RemoveAll(tmp);
  if (::mkdir(tmp.c_str(), 0755) != 0) return std::unexpected(Errno(tmp));
  const auto fail =
      [&](const std::string& why) -> std::expected<InstallResult, std::string> {
    RemoveAll(tmp);
    return std::unexpected(package + ": " + why);
  };

  {
    DirectorySink sink{tmp};
    if (auto ok = Unpack(fd.get(), verified->footer, sink); !ok) {
      return fail(ok.error());
    }
  }
  const auto config_text =
      ReadText(tmp + "/" + std::string{kConfigFile}, kMaxTextSize);
  const auto config =
      nlohmann::json::parse(config_text.value_or(""), nullptr, false);
  if (!config.is_object()) return fail("config.json is not a JSON object");
  const auto config_name = config.find("name");
  if (config_name == config.end() || !config_name->is_string() ||
      config_name->get<std::string>() != name) {
    return fail("config.json's \"name\" is not '" + name + "'");
  }
  if (options.check_config) {
    if (auto ok = options.check_config(config); !ok) {
      return fail("config.json: " + ok.error());
    }
  }

  nlohmann::json installed{
      {"name", name}, {"digest", result.digest}, {"signed_by", nullptr}};
  if (result.signed_by) installed["signed_by"] = *result.signed_by;
  if (auto ok = WriteSynced(tmp + "/" + std::string{kInstalledFile},
                            installed.dump(2) + "\n");
      !ok) {
    return fail(ok.error());
  }
  if (auto ok = SyncDirectory(tmp); !ok) return fail(ok.error());

  // The swap: dest is never missing. The old version stays as <name>.prev.
  const std::string prev = dest + ".prev";
  if (Exists(dest)) {
    if (auto ok = Exchange(tmp, dest); !ok) return fail(ok.error());
    RemoveAll(prev);
    if (std::rename(tmp.c_str(), prev.c_str()) != 0) {
      RemoveAll(tmp);  // the old version; the new one is in place
    }
  } else if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
    return fail(Errno("rename to " + dest));
  }
  if (auto ok = SyncDirectory(root); !ok) return std::unexpected(ok.error());
  if (auto ok = remove_package(); !ok) return std::unexpected(ok.error());
  result.changed = true;
  return result;
}

std::expected<void, std::string> Rollback(const std::string& root,
                                          std::string_view name) {
  if (!ValidName(name)) {
    return std::unexpected("'" + std::string{name} + "' is not a valid name");
  }
  const std::string dest = root + "/" + std::string{name};
  const std::string prev = dest + ".prev";
  if (!Exists(prev)) {
    return std::unexpected("no previous version of " + std::string{name});
  }
  if (!Exists(dest)) {
    if (std::rename(prev.c_str(), dest.c_str()) != 0) {
      return std::unexpected(Errno("rename " + prev));
    }
  } else if (auto ok = Exchange(prev, dest); !ok) {
    return ok;
  }
  return SyncDirectory(root);
}

void Recover(const std::string& root, std::vector<std::string>& notes) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  std::vector<std::string> entries;
  for (const auto& entry : fs::directory_iterator{root, ec}) {
    entries.push_back(entry.path().filename().string());
  }
  std::ranges::sort(entries);
  bool changed = false;
  const auto at = [&](std::string_view entry) {
    return (fs::path{root} / entry).string();
  };
  const auto rename = [&](const std::string& from, const std::string& to) {
    if (std::rename(at(from).c_str(), at(to).c_str()) == 0) {
      std::string note = from;
      note += " -> ";
      note += to;
      notes.push_back(std::move(note));
      changed = true;
    }
  };

  // A missing <name> comes back first: the complete new version, else the
  // previous one, else what an interrupted exchange left.
  for (const auto& e : entries) {
    if (!e.starts_with('.') || !e.ends_with(".new")) continue;
    const auto name = e.substr(1, e.size() - 5);
    if (!ValidName(name) || Exists(at(name))) continue;
    if (Exists((fs::path{at(e)} / kInstalledFile).string())) {
      rename(e, name);
    }
  }
  for (const auto& e : entries) {
    for (const std::string_view suffix : {".exchange", ".prev"}) {
      if (!e.ends_with(suffix)) continue;
      const auto name = e.substr(0, e.size() - suffix.size());
      if (!ValidName(name) || !Exists(at(e))) continue;
      if (!Exists(at(name))) {
        rename(e, name);
      } else if (suffix == ".exchange") {
        const auto prev = name + ".prev";
        RemoveAll(at(prev));
        rename(e, prev);  // the old version of an exchange
      }
    }
  }
  // Unpacks that never finished.
  for (const auto& e : entries) {
    if (!e.starts_with('.') || !e.ends_with(".new")) continue;
    if (!Exists(at(e))) continue;
    RemoveAll(at(e));
    notes.push_back("removed " + e);
    changed = true;
  }
  if (changed) std::ignore = SyncDirectory(root);
}

}  // namespace aember::utils::package
