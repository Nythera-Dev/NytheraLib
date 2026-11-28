#include "render/core/thumbnail_service.h"

#include "core/files/directory_scanner.h"
#include "core/log.h"
#include "render/core/freedesktop_thumbnail.h"
#include "render/core/image_decoder.h"
#include "render/core/image_resize.h"
#include "render/core/svg_rasterizer.h"
#include "render/core/texture_manager.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <glib.h>
#include <limits>
#include <optional>
#include <random>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
#include <webp/encode.h>

namespace {

  static_assert(ThumbnailService::kMaxSvgSourceBytes == svg_rasterizer::kMaxSourceBytes);

  constexpr Logger kLog("thumbnail");
  constexpr float kThumbnailWebPQuality = 82.0F;
  constexpr std::size_t kMinWorkers = 2;
  constexpr std::size_t kMaxWorkers = 4;
  constexpr std::string_view kThumbnailCacheVersion = "thumbnail-service-v6";

  std::mutex g_testHookMutex;
  std::function<bool(const std::string&, const std::string&)> g_cacheWriteTestHook;
  std::function<void(const std::string&, std::size_t)> g_decodeObserverHook;

  class OwnedFd {
  public:
    OwnedFd() = default;
    explicit OwnedFd(int fd) noexcept : m_fd(fd) {}
    ~OwnedFd() { reset(); }

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;

    OwnedFd(OwnedFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
      if (this != &other) {
        reset();
        m_fd = std::exchange(other.m_fd, -1);
      }
      return *this;
    }

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_fd >= 0; }

    int release() noexcept { return std::exchange(m_fd, -1); }

    bool closeChecked() noexcept {
      if (m_fd < 0) {
        return true;
      }
      const int fd = std::exchange(m_fd, -1);
      return ::close(fd) == 0;
    }

    void reset() noexcept {
      if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
      }
    }

  private:
    int m_fd = -1;
  };

  std::optional<std::filesystem::path> thumbnailCacheDir(int pngSize = 0) {
    const auto subdir = pngSize == 0 ? std::filesystem::path("nythera/thumbnails")
                                     : std::filesystem::path("thumbnails") / freedesktop_thumbnail::directory(pngSize);
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      const std::filesystem::path base(xdg);
      if (base.is_absolute()) {
        return base / subdir;
      }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      const std::filesystem::path base(home);
      if (base.is_absolute()) {
        return base / ".cache" / subdir;
      }
    }
    // Never fall back to a shared /tmp directory when no user cache directory is configured.
    return std::nullopt;
  }

  OwnedFd openSharedCacheDir(const std::filesystem::path& dir, bool create) {
    const auto ensureOwnerOnly = [](const OwnedFd& fd) {
      struct stat info{};
      return fd
          && ::fstat(fd.get(), &info) == 0
          && info.st_uid == ::geteuid()
          && ((info.st_mode & 07777) == 0700 || ::fchmod(fd.get(), 0700) == 0);
    };
    const auto root = dir.parent_path();
    if (create) {
      std::error_code ec;
      std::filesystem::create_directories(root.parent_path(), ec);
      if (ec || (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST))
        return {};
    }
    OwnedFd parent(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!ensureOwnerOnly(parent))
      return {};

    const auto leaf = dir.filename();
    if (create && ::mkdirat(parent.get(), leaf.c_str(), 0700) != 0 && errno != EEXIST)
      return {};
    OwnedFd bucket(::openat(parent.get(), leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!ensureOwnerOnly(bucket))
      return {};
    return bucket;
  }

  OwnedFd openPrivateCacheDir(bool create, int pngSize = 0) {
    const auto dir = thumbnailCacheDir(pngSize);
    if (!dir.has_value()) {
      return {};
    }

    if (pngSize != 0)
      return openSharedCacheDir(*dir, create);

    if (create) {
      std::error_code ec;
      if (!FileUtils::createPrivateDirectories(*dir, ec)) {
        return {};
      }
    }

    OwnedFd dirFd(::open(dir->c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dirFd) {
      return {};
    }

    struct stat st{};
    if (::fstat(dirFd.get(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
      return {};
    }

    if ((st.st_mode & 0077) != 0) {
      if (::fchmod(dirFd.get(), 0700) != 0) {
        return {};
      }
      if (::fstat(dirFd.get(), &st) != 0 || (st.st_mode & 0077) != 0) {
        return {};
      }
    }

    return dirFd;
  }

  std::string sha256Hex(std::string_view text) {
    g_autofree gchar* digest =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(text.data()), text.size());
    return digest != nullptr ? std::string(digest) : std::string{};
  }

  std::string cacheLeafForStat(const std::string& sourcePath, const struct stat& st, int targetPx) {
    const std::string key = sourcePath
        + '\n'
        + std::to_string(static_cast<long long>(st.st_size))
        + '\n'
        + std::to_string(static_cast<long long>(st.st_mtim.tv_sec))
        + '.'
        + std::to_string(static_cast<long long>(st.st_mtim.tv_nsec))
        + '\n'
        + std::to_string(targetPx)
        + '\n'
        + std::string(kThumbnailCacheVersion);
    const std::string digest = sha256Hex(key);
    if (digest.empty()) {
      return {};
    }
    return digest + ".webp";
  }

  bool inspectRegularSource(const std::string& sourcePath, struct stat& st, bool allowLarge) {
    if (sourcePath.empty()) {
      return false;
    }
    if (::lstat(sourcePath.c_str(), &st) != 0) {
      return false;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
      return false;
    }
    return allowLarge || static_cast<std::uint64_t>(st.st_size) <= ThumbnailService::kMaxSourceFileBytes;
  }

  OwnedFd openRegularSource(const std::string& sourcePath, struct stat& st, bool allowLarge) {
    if (sourcePath.empty()) {
      return {};
    }
    OwnedFd fd(::open(sourcePath.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
      return {};
    }
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
      return {};
    }
    if (!allowLarge && static_cast<std::uint64_t>(st.st_size) > ThumbnailService::kMaxSourceFileBytes) {
      return {};
    }
    return fd;
  }

  bool sourceStillCurrent(const std::string& path, const struct stat& opened) {
    struct stat current{};
    return ::lstat(path.c_str(), &current) == 0
        && S_ISREG(current.st_mode)
        && current.st_dev == opened.st_dev
        && current.st_ino == opened.st_ino
        && current.st_size == opened.st_size
        && current.st_mtim.tv_sec == opened.st_mtim.tv_sec
        && current.st_mtim.tv_nsec == opened.st_mtim.tv_nsec;
  }

  std::vector<std::uint8_t> readBoundedFd(int fd, std::size_t maxBytes, std::size_t reserveHint) {
    std::vector<std::uint8_t> out;
    out.reserve(std::min(reserveHint, maxBytes));

    std::array<std::uint8_t, 65536> chunk{};
    while (true) {
      const ssize_t n = ::read(fd, chunk.data(), chunk.size());
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return {};
      }
      if (n == 0) {
        break;
      }
      const auto count = static_cast<std::size_t>(n);
      if (out.size() > maxBytes || count > maxBytes - out.size()) {
        return {};
      }
      out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return out;
  }

  std::vector<std::uint8_t> readCacheEntry(int dirFd, const std::string& leaf) {
    if (dirFd < 0 || leaf.empty()) {
      return {};
    }
    OwnedFd fd(::openat(dirFd, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
      return {};
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0
        || !S_ISREG(st.st_mode)
        || st.st_uid != ::geteuid()
        || st.st_size <= 0
        || static_cast<std::uint64_t>(st.st_size) > ThumbnailService::kMaxCacheFileBytes) {
      return {};
    }
    return readBoundedFd(fd.get(), ThumbnailService::kMaxCacheFileBytes, static_cast<std::size_t>(st.st_size));
  }

  void removeCorruptCacheEntry(int dirFd, const std::string& leaf) {
    if (dirFd < 0 || leaf.empty()) {
      return;
    }
    struct stat st{};
    if (::fstatat(dirFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(st.st_mode)
        && st.st_uid == ::geteuid()) {
      (void)::unlinkat(dirFd, leaf.c_str(), 0);
    }
  }

  bool writeAllFd(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t n = ::write(fd, data + offset, size - offset);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (n == 0) {
        return false;
      }
      offset += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool writeCacheEntryAtomic(int dirFd, const std::string& leaf, const std::uint8_t* data, std::size_t size) {
    if (dirFd < 0 || leaf.empty() || data == nullptr || size == 0) {
      return false;
    }

    struct stat existing{};
    if (::fstatat(dirFd, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
      if (!S_ISREG(existing.st_mode) || existing.st_uid != ::geteuid()) {
        return false;
      }
    } else if (errno != ENOENT) {
      return false;
    }

    thread_local std::mt19937_64 rng(std::random_device{}());
    const auto pid = static_cast<unsigned long>(::getpid());

    OwnedFd tempFd;
    std::string tempName;
    for (int attempt = 0; attempt < 32; ++attempt) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), ".tmp-%016llx-%lu.webp", static_cast<unsigned long long>(rng()), pid);
      tempName = buf;
      const int fd = ::openat(dirFd, tempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd >= 0) {
        tempFd = OwnedFd(fd);
        break;
      }
      if (errno != EEXIST) {
        return false;
      }
    }
    if (!tempFd) {
      return false;
    }

    bool ok = (::fchmod(tempFd.get(), 0600) == 0) && writeAllFd(tempFd.get(), data, size);
    if (!tempFd.closeChecked()) {
      ok = false;
    }

    if (ok) {
      std::function<bool(const std::string&, const std::string&)> hook;
      {
        std::scoped_lock lock(g_testHookMutex);
        hook = g_cacheWriteTestHook;
      }
      if (hook && !hook(tempName, leaf)) {
        ok = false;
      }
    }

    if (ok) {
      if (::fstatat(dirFd, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0
          && (!S_ISREG(existing.st_mode) || existing.st_uid != ::geteuid())) {
        ok = false;
      }
    }

    if (ok && ::renameat(dirFd, tempName.c_str(), dirFd, leaf.c_str()) != 0) {
      ok = false;
    }

    if (!ok) {
      (void)::unlinkat(dirFd, tempName.c_str(), 0);
      return false;
    }
    return true;
  }

  // Keep straight alpha; the shared resampler weights colour by alpha when filtering.
  bool resizeThumbnail(std::vector<std::uint8_t>& rgba, int& width, int& height, int targetPx) {
    const ImageExtent fitted = fitImageWithin(width, height, targetPx, targetPx);
    if (fitted.width == width && fitted.height == height) {
      return true;
    }

    auto resized = resizeToRgba(rgba.data(), width, height, ResizeSourceLayout::Rgba, fitted.width, fitted.height);
    if (resized.empty()) {
      return false;
    }
    rgba = std::move(resized);
    width = fitted.width;
    height = fitted.height;
    return true;
  }

} // namespace

std::size_t ThumbnailService::RequestKeyHash::operator()(const RequestKey& key) const noexcept {
  const std::size_t pathHash = std::hash<std::string>{}(key.path);
  const std::size_t sizeHash = std::hash<int>{}(key.targetPx);
  return pathHash ^ (sizeHash + 0x9e3779b97f4a7c15ULL + (pathHash << 6U) + (pathHash >> 2U));
}

ThumbnailService::Subscription::Subscription(std::function<void()> disconnect) : m_disconnect(std::move(disconnect)) {}

ThumbnailService::Subscription::~Subscription() { disconnect(); }

ThumbnailService::Subscription::Subscription(Subscription&& other) noexcept
    : m_disconnect(std::move(other.m_disconnect)) {}

ThumbnailService::Subscription& ThumbnailService::Subscription::operator=(Subscription&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  disconnect();
  m_disconnect = std::move(other.m_disconnect);
  return *this;
}

void ThumbnailService::Subscription::disconnect() {
  if (!m_disconnect) {
    return;
  }
  auto disconnect = std::move(m_disconnect);
  m_disconnect = nullptr;
  disconnect();
}

struct ThumbnailService::CacheMaintenance {
  using Clock = std::chrono::steady_clock;
  std::optional<Clock::time_point> nextPrune;
  std::size_t budget = 0;
  bool running = false;
  struct Request {
    bool clear;
    std::function<void(DiskCacheInfo)> callback;
  };
  std::vector<Request> requests;
  std::vector<std::function<void()>> completions;
};

void ThumbnailService::setSourceFileLimit(std::size_t bytes) {
  m_sourceLimit.store(std::clamp<std::size_t>(bytes, 1, kMaxSourceFileBytes));
}

void ThumbnailService::setFreedesktopCacheBudget(std::size_t bytes) {
  if (!m_cacheMaintenance)
    return;
  {
    std::scoped_lock lock(m_queueMutex);
    if (m_cacheMaintenance->budget == bytes)
      return;
    m_cacheMaintenance->budget = bytes;
    if (bytes == 0)
      m_cacheMaintenance->nextPrune.reset();
  }
  scheduleCachePrune();
}

void ThumbnailService::scheduleCachePrune() {
  {
    std::scoped_lock lock(m_queueMutex);
    auto& cache = *m_cacheMaintenance;
    if (cache.budget == 0 || cache.nextPrune)
      return;
    cache.nextPrune = CacheMaintenance::Clock::now() + std::chrono::milliseconds(250);
  }
  m_queueCv.notify_all();
}

void ThumbnailService::requestFreedesktopCacheInfo(bool clear, std::function<void(DiskCacheInfo)> callback) {
  if (!m_cacheMaintenance || !callback)
    return;
  {
    std::scoped_lock lock(m_queueMutex);
    m_cacheMaintenance->requests.push_back({clear, std::move(callback)});
  }
  m_queueCv.notify_all();
}

ThumbnailService::DiskCacheInfo ThumbnailService::maintainFreedesktopCache(bool clear, std::size_t budget) {
  struct CachedFile {
    int bucket;
    std::string name;
    struct stat info;
  };
  std::array<OwnedFd, std::size(freedesktop_thumbnail::sizes)> buckets;
  std::vector<CachedFile> files;
  DiskCacheInfo total;
  const auto remove = [](const CachedFile& file) {
    struct stat current{};
    return ::fstatat(file.bucket, file.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
        && current.st_dev == file.info.st_dev
        && current.st_ino == file.info.st_ino
        && current.st_mtim.tv_sec == file.info.st_mtim.tv_sec
        && current.st_mtim.tv_nsec == file.info.st_mtim.tv_nsec
        && ::unlinkat(file.bucket, file.name.c_str(), 0) == 0;
  };
  for (std::size_t i = 0; i < buckets.size() && !m_shutdown.load(); ++i) {
    auto& bucket = buckets[i];
    bucket = openPrivateCacheDir(false, freedesktop_thumbnail::sizes[i]);
    if (!bucket)
      continue;
    OwnedFd scanFd(::fcntl(bucket.get(), F_DUPFD_CLOEXEC, 0));
    if (!scanFd)
      continue;
    auto* stream = ::fdopendir(scanFd.get());
    if (!stream)
      continue;
    (void)scanFd.release();
    while (!m_shutdown.load()) {
      const auto* entry = ::readdir(stream);
      if (!entry)
        break;
      const std::string_view name(entry->d_name);
      if (name.size() != 36
          || !name.ends_with(".png")
          || name.substr(0, 32).find_first_not_of("0123456789abcdef") != std::string_view::npos)
        continue;
      CachedFile file{bucket.get(), std::string(name), {}};
      if (::fstatat(file.bucket, file.name.c_str(), &file.info, AT_SYMLINK_NOFOLLOW) != 0
          || !S_ISREG(file.info.st_mode)
          || file.info.st_uid != ::geteuid()
          || file.info.st_size <= 0)
        continue;
      if (clear
          && freedesktop_thumbnail::writtenBy(readCacheEntry(file.bucket, file.name), m_cacheSoftware)
          && remove(file))
        continue;
      total.bytes += static_cast<std::uintmax_t>(file.info.st_size);
      ++total.thumbnails;
      if (budget)
        files.push_back(std::move(file));
    }
    ::closedir(stream);
  }
  if (budget && total.bytes > budget) {
    std::ranges::sort(files, [](const auto& a, const auto& b) {
      return std::pair{a.info.st_mtim.tv_sec, a.info.st_mtim.tv_nsec}
      < std::pair{b.info.st_mtim.tv_sec, b.info.st_mtim.tv_nsec};
    });
    for (const auto& file : files) {
      if (m_shutdown.load() || total.bytes <= budget)
        break;
      if (remove(file)) {
        total.bytes -= static_cast<std::uintmax_t>(file.info.st_size);
        --total.thumbnails;
      }
    }
  }
  return total;
}

ThumbnailService::ThumbnailService() : ThumbnailService(CacheBackend::PrivateWebP) {}

ThumbnailService::ThumbnailService(CacheBackend backend) : ThumbnailService(backend, {}) {}

ThumbnailService::ThumbnailService(CacheBackend backend, Provider provider, std::string cacheSoftware)
    : m_cacheBackend(backend), m_provider(std::move(provider)), m_cacheSoftware(std::move(cacheSoftware)),
      m_cacheMaintenance(backend == CacheBackend::FreedesktopPng ? std::make_unique<CacheMaintenance>() : nullptr) {
  m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_eventFd < 0) {
    kLog.warn("failed to create eventfd; thumbnail wakeups will be disabled");
  }

  const unsigned hc = std::thread::hardware_concurrency();
  const std::size_t suggested = (hc == 0) ? kMinWorkers : std::max<std::size_t>(kMinWorkers, hc / 2);
  const std::size_t n = std::clamp<std::size_t>(suggested, kMinWorkers, kMaxWorkers);
  m_workers.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    m_workers.emplace_back([this] { workerLoop(); });
  }
  kLog.info("spawned {} decode worker(s)", n);
}

ThumbnailService::~ThumbnailService() {
  if (m_lifetimeToken != nullptr) {
    *m_lifetimeToken = false;
  }
  m_pendingListeners.clear();
  m_readyListeners.clear();

  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  {
    std::scoped_lock lock(m_memoryMutex);
  }
  m_memoryCv.notify_all();
  for (auto& t : m_workers) {
    if (t.joinable()) {
      t.join();
    }
  }

  deleteAllTextures();

  if (m_eventFd >= 0) {
    ::close(m_eventFd);
    m_eventFd = -1;
  }
}

ThumbnailService::Subscription ThumbnailService::subscribePendingUpload(PendingUploadCallback callback) {
  if (!callback) {
    return {};
  }

  const std::uint64_t id = m_nextListenerId++;
  m_pendingListeners.emplace(id, PendingListener{.callback = std::move(callback)});
  bool hasPendingResults = false;
  {
    std::scoped_lock lock(m_resultMutex);
    hasPendingResults = !m_results.empty();
  }
  if (hasPendingResults && m_pendingListeners.contains(id) && m_pendingListeners[id].callback) {
    m_pendingListeners[id].callback();
  }

  std::weak_ptr<bool> token = m_lifetimeToken;
  return Subscription([this, token, id] {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_pendingListeners.erase(id);
  });
}

ThumbnailService::Subscription
ThumbnailService::subscribeReady(const std::string& path, ReadyCallback callback, int targetPx) {
  if (path.empty() || !callback) {
    return {};
  }

  const TextureHandle current = peek(path, targetPx);
  if (current.id != 0) {
    callback(path, current);
    return {};
  }

  const std::uint64_t id = m_nextListenerId++;
  m_readyListeners.emplace(
      id, ReadyListener{.key = RequestKey{.path = path, .targetPx = targetPx}, .callback = std::move(callback)}
  );

  std::weak_ptr<bool> token = m_lifetimeToken;
  return Subscription([this, token, id] {
    auto alive = token.lock();
    if (alive == nullptr || !*alive) {
      return;
    }
    m_readyListeners.erase(id);
  });
}

TextureHandle ThumbnailService::acquire(const std::string& path, int targetPx, bool diskCache) {
  if (path.empty()) {
    return {};
  }

  const RequestKey key{.path = path, .targetPx = targetPx, .diskCache = diskCache};
  CacheEntry& entry = m_entries[key];
  ++entry.refCount;
  if (entry.handle.id != 0 || entry.failed) {
    return entry.handle;
  }

  enqueueDecodeIfNeeded(key);
  return {};
}

TextureHandle ThumbnailService::peek(const std::string& path, int targetPx, bool diskCache) const {
  const auto it = m_entries.find(RequestKey{.path = path, .targetPx = targetPx, .diskCache = diskCache});
  if (it == m_entries.end()) {
    return {};
  }
  return it->second.handle;
}

bool ThumbnailService::failed(const std::string& path, int targetPx, bool diskCache) const {
  const auto it = m_entries.find(RequestKey{.path = path, .targetPx = targetPx, .diskCache = diskCache});
  return it != m_entries.end() && it->second.failed;
}

void ThumbnailService::release(const std::string& path, int targetPx, bool diskCache) {
  const RequestKey key{.path = path, .targetPx = targetPx, .diskCache = diskCache};
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return;
  }

  CacheEntry& entry = it->second;
  if (entry.refCount > 0) {
    --entry.refCount;
  }

  if (entry.refCount > 0) {
    return;
  }

  if (entry.handle.id != 0 && m_textureManager != nullptr) {
    m_textureManager->unload(entry.handle);
  }
  m_entries.erase(it);

  bool canceledInFlight = false;
  {
    std::scoped_lock lock(m_queueMutex);
    if (m_inFlight.contains(key)) {
      m_canceled.insert(key);
      canceledInFlight = true;
    }
  }
  if (canceledInFlight) {
    {
      std::scoped_lock lock(m_memoryMutex);
    }
    m_memoryCv.notify_all();
  }
}

bool ThumbnailService::isCanceledOrShuttingDown(const RequestKey& key) const {
  if (m_shutdown.load()) {
    return true;
  }
  std::scoped_lock lock(m_queueMutex);
  return m_canceled.contains(key);
}

bool ThumbnailService::tryAbortIfCanceled(const RequestKey& key) {
  std::scoped_lock lock(m_queueMutex);
  if (m_shutdown.load()) {
    m_canceled.erase(key);
    m_inFlight.erase(key);
    return true;
  }
  if (m_canceled.erase(key) > 0) {
    m_inFlight.erase(key);
    return true;
  }
  return false;
}

bool ThumbnailService::acquireMemoryBudget(const RequestKey& key, std::size_t bytes) {
  std::unique_lock<std::mutex> lock(m_memoryMutex);
  const std::size_t reservation = std::min(bytes, m_memoryBudgetBytes);
  m_memoryCv.wait(lock, [this, &key, reservation] {
    return isCanceledOrShuttingDown(key)
        || m_usedMemoryBytes == 0
        || m_usedMemoryBytes + reservation <= m_memoryBudgetBytes;
  });
  if (isCanceledOrShuttingDown(key)) {
    return false;
  }
  m_usedMemoryBytes += reservation;
  return true;
}

void ThumbnailService::releaseMemoryBudget(std::size_t bytes) {
  {
    std::scoped_lock lock(m_memoryMutex);
    const std::size_t reservation = std::min(bytes, m_memoryBudgetBytes);
    m_usedMemoryBytes = m_usedMemoryBytes > reservation ? (m_usedMemoryBytes - reservation) : 0;
  }
  m_memoryCv.notify_all();
}

void ThumbnailService::enqueueDecodeIfNeeded(const RequestKey& key) {
  std::scoped_lock lock(m_queueMutex);
  m_canceled.erase(key);
  if (m_inFlight.contains(key)) {
    return;
  }
  m_inFlight.insert(key);
  if (m_cacheBackend == CacheBackend::FreedesktopPng && !key.diskCache)
    m_jobQueue.push_front(key);
  else
    m_jobQueue.push_back(key);
  m_queueCv.notify_one();
}

bool ThumbnailService::uploadPending(TextureManager& textures) {
  m_textureManager = &textures;

  std::deque<DecodedJob> jobs;
  {
    std::scoped_lock lock(m_resultMutex);
    jobs = std::move(m_results);
    m_results.clear();
  }
  if (jobs.empty()) {
    return false;
  }

  const auto alive = m_lifetimeToken;
  bool changed = false;
  for (auto& job : jobs) {
    if (!*alive) {
      return changed;
    }
    bool dropped = false;
    {
      std::scoped_lock lock(m_queueMutex);
      m_inFlight.erase(job.key);
      if (auto c = m_canceled.find(job.key); c != m_canceled.end()) {
        m_canceled.erase(c);
        dropped = true;
      }
    }
    if (dropped) {
      continue;
    }

    auto entryIt = m_entries.find(job.key);
    if (entryIt == m_entries.end() || entryIt->second.refCount == 0) {
      continue;
    }
    if (job.failed || job.rgba.empty() || job.width <= 0 || job.height <= 0) {
      entryIt->second.failed = true;
      changed = true;
      continue;
    }

    TextureHandle handle = textures.loadFromRgba(job.rgba.data(), job.width, job.height);
    if (handle.id == 0) {
      kLog.warn("failed to upload thumbnail texture for {}", job.key.path);
      entryIt->second.failed = true;
      changed = true;
      continue;
    }

    if (entryIt->second.handle.id != 0 && m_textureManager != nullptr) {
      m_textureManager->unload(entryIt->second.handle);
    }
    entryIt->second.handle = handle;
    entryIt->second.failed = false;
    changed = true;
    notifyReady(job.key, handle);
    if (!*alive) {
      return changed;
    }
  }
  return changed;
}

void ThumbnailService::invalidateGpuResources(TextureManager& textures) {
  m_textureManager = &textures;

  std::vector<RequestKey> liveKeys;
  liveKeys.reserve(m_entries.size());
  for (auto& [key, entry] : m_entries) {
    if (entry.handle.id != 0) {
      m_textureManager->unload(entry.handle);
    }
    entry.handle = {};
    entry.failed = false;
    if (entry.refCount > 0) {
      liveKeys.push_back(key);
    }
  }

  for (const RequestKey& key : liveKeys) {
    enqueueDecodeIfNeeded(key);
  }
}

void ThumbnailService::abandonGpuResources() noexcept {
  for (auto& [key, entry] : m_entries) {
    (void)key;
    entry.handle = {};
    entry.failed = false;
  }
  m_textureManager = nullptr;
}

void ThumbnailService::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_eventFd < 0) {
    return;
  }
  fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0});
}

void ThumbnailService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_eventFd < 0 || startIdx >= fds.size()) {
    return;
  }
  if ((fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  std::uint64_t ignored = 0;
  while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
  }

  std::vector<std::function<void()>> completions;
  if (m_cacheMaintenance) {
    std::scoped_lock lock(m_queueMutex);
    completions.swap(m_cacheMaintenance->completions);
  }
  const std::weak_ptr<bool> alive = m_lifetimeToken;
  for (auto& callback : completions) {
    if (alive.expired())
      return;
    callback();
  }
  if (!alive.expired())
    notifyPendingUpload();
}

void ThumbnailService::signalMain() {
  if (m_eventFd < 0) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
  if (written < 0 && errno != EAGAIN) {
    kLog.warn("failed to signal thumbnail eventfd: errno={}", errno);
  }
}

void ThumbnailService::pushResult(DecodedJob job) {
  {
    std::scoped_lock lock(m_resultMutex);
    m_results.push_back(std::move(job));
  }
  signalMain();
}

void ThumbnailService::deleteAllTextures() {
  for (auto& [key, entry] : m_entries) {
    (void)key;
    if (entry.handle.id != 0 && m_textureManager != nullptr) {
      m_textureManager->unload(entry.handle);
    }
  }
  m_entries.clear();
}

void ThumbnailService::notifyPendingUpload() {
  const auto alive = m_lifetimeToken;
  std::vector<std::pair<std::uint64_t, PendingUploadCallback>> callbacks;
  callbacks.reserve(m_pendingListeners.size());
  for (const auto& [id, listener] : m_pendingListeners) {
    if (listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (auto& [id, callback] : callbacks) {
    if (!*alive) {
      return;
    }
    if (!m_pendingListeners.contains(id)) {
      continue;
    }
    callback();
  }
}

void ThumbnailService::notifyReady(const RequestKey& key, TextureHandle handle) {
  const auto alive = m_lifetimeToken;
  std::vector<std::pair<std::uint64_t, ReadyCallback>> callbacks;
  for (const auto& [id, listener] : m_readyListeners) {
    if (listener.key == key && listener.callback) {
      callbacks.emplace_back(id, listener.callback);
    }
  }

  for (const auto& [id, callback] : callbacks) {
    if (!*alive) {
      return;
    }
    const auto it = m_readyListeners.find(id);
    if (it == m_readyListeners.end()) {
      continue;
    }
    m_readyListeners.erase(it);
    callback(key.path, handle);
  }
}

void ThumbnailService::workerLoop() {
  while (true) {
    RequestKey key;
    std::vector<CacheMaintenance::Request> maintenance;
    std::size_t pruneBudget = 0;
    bool maintain = false;
    {
      std::unique_lock lock(m_queueMutex);
      while (!m_shutdown.load()) {
        auto* cache = m_cacheMaintenance.get();
        if (cache && !cache->running) {
          const bool prune = cache->nextPrune && CacheMaintenance::Clock::now() >= *cache->nextPrune;
          if (prune || !cache->requests.empty()) {
            maintain = cache->running = true;
            maintenance.swap(cache->requests);
            if (prune) {
              pruneBudget = cache->budget;
              cache->nextPrune.reset();
            }
            break;
          }
        }
        if (!m_jobQueue.empty())
          break;
        if (cache && !cache->running && cache->nextPrune) {
          const auto deadline = *cache->nextPrune;
          m_queueCv.wait_until(lock, deadline);
        } else {
          m_queueCv.wait(lock);
        }
      }
      if (m_shutdown.load())
        return;
      if (!maintain) {
        key = std::move(m_jobQueue.front());
        m_jobQueue.pop_front();
        if (m_canceled.erase(key) > 0) {
          m_inFlight.erase(key);
          continue;
        }
      }
    }
    if (maintain) {
      std::vector<std::function<void()>> completions;
      if (pruneBudget)
        (void)maintainFreedesktopCache(false, pruneBudget);
      for (auto& request : maintenance) {
        const auto info = maintainFreedesktopCache(request.clear, 0);
        completions.push_back([callback = std::move(request.callback), info] { callback(info); });
      }
      {
        std::scoped_lock lock(m_queueMutex);
        m_cacheMaintenance->running = false;
        std::ranges::move(completions, std::back_inserter(m_cacheMaintenance->completions));
      }
      if (!completions.empty())
        signalMain();
      m_queueCv.notify_all();
      continue;
    }

    const std::string& path = key.path;
    DecodedJob result;
    result.key = key;

    struct stat inspectedStat{};
    if (!inspectRegularSource(path, inspectedStat, static_cast<bool>(m_provider))) {
      if (tryAbortIfCanceled(key)) {
        continue;
      }
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    const bool webpCache = key.diskCache && m_cacheBackend == CacheBackend::PrivateWebP;
    const int pngSize = key.diskCache && m_cacheBackend == CacheBackend::FreedesktopPng
        ? freedesktop_thumbnail::sizeClass(key.targetPx)
        : 0;
    if (OwnedFd cacheDir = webpCache ? openPrivateCacheDir(false) : OwnedFd{}; cacheDir) {
      const std::string leaf = cacheLeafForStat(path, inspectedStat, key.targetPx);
      auto cachedBytes = readCacheEntry(cacheDir.get(), leaf);
      if (!cachedBytes.empty()) {
        const RasterDecodeLimits cacheDecodeLimits{
            .maxDimension = std::max(key.targetPx * 2, 2048),
            .maxDecodedBytes = kMaxCacheFileBytes * 4ULL,
        };
        if (auto cached = decodeRasterImage(cachedBytes.data(), cachedBytes.size(), cacheDecodeLimits)) {
          if (tryAbortIfCanceled(key)) {
            continue;
          }
          result.rgba = std::move(cached->pixels);
          result.width = cached->width;
          result.height = cached->height;
          pushResult(std::move(result));
          continue;
        }
        removeCorruptCacheEntry(cacheDir.get(), leaf);
      }
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    struct stat openedStat{};
    OwnedFd sourceFd = openRegularSource(path, openedStat, static_cast<bool>(m_provider));
    if (!sourceFd) {
      if (tryAbortIfCanceled(key)) {
        continue;
      }
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    const auto pngDir = pngSize != 0 ? thumbnailCacheDir(pngSize) : std::nullopt;
    const auto relativeToCache = pngDir
        ? std::filesystem::path(path).lexically_normal().lexically_relative(pngDir->parent_path())
        : std::filesystem::path{};
    const bool insideCache = !relativeToCache.empty() && *relativeToCache.begin() != "..";
    const auto pngIdentity = pngDir && !insideCache ? freedesktop_thumbnail::identify(path, openedStat) : std::nullopt;
    const int decodeTarget = pngIdentity ? pngSize : key.targetPx;
    if (pngIdentity) {
      std::array<int, 4> cacheSizes{128, 256, 512, 1024};
      // Prefer the requested bucket, then higher resolutions. Keep a smaller
      // hit as a fallback if we cannot regenerate from the source.
      auto bucket = std::ranges::find(cacheSizes, pngSize);
      std::reverse(cacheSizes.begin(), bucket);
      std::rotate(cacheSizes.begin(), bucket, cacheSizes.end());
      bool satisfied = false;
      for (const int side : cacheSizes) {
        const auto directory = openPrivateCacheDir(false, side);
        if (!directory)
          continue;
        const auto bytes = readCacheEntry(directory.get(), pngIdentity->leaf);
        const auto match = freedesktop_thumbnail::match(bytes, *pngIdentity, side);
        if (match == freedesktop_thumbnail::Match::Invalid)
          continue;
        auto cached = decodeRasterImage(
            bytes.data(), bytes.size(), {.maxDimension = side, .maxDecodedBytes = 8ULL * side * side + 65536}
        );
        if (!cached || !resizeThumbnail(cached->pixels, cached->width, cached->height, key.targetPx))
          continue;
        if (!sourceStillCurrent(path, openedStat))
          continue;
        const int cachedSide = std::max(cached->width, cached->height);
        const bool sufficient = cachedSide >= key.targetPx || match == freedesktop_thumbnail::Match::OriginalSize;
        if (sufficient || cachedSide > std::max(result.width, result.height)) {
          result.rgba = std::move(cached->pixels);
          result.width = cached->width;
          result.height = cached->height;
        }
        // An application provider may already have tried this resolution. Its
        // smaller output is reusable once an adequate bucket has been rendered.
        satisfied |= sufficient || (side >= pngSize && match == freedesktop_thumbnail::Match::RenderedSize);
        if (sufficient)
          break;
      }
      if (satisfied && sourceStillCurrent(path, openedStat)) {
        if (!tryAbortIfCanceled(key))
          pushResult(std::move(result));
        continue;
      }
    }

    const auto finishWithoutSourceImage = [&] {
      if (tryAbortIfCanceled(key))
        return;
      if (result.rgba.empty() || !sourceStillCurrent(path, openedStat)) {
        result.rgba.clear();
        result.failed = true;
      }
      pushResult(std::move(result));
    };
    const auto sourceSize = static_cast<std::size_t>(openedStat.st_size);
    const auto sourceLimit = m_sourceLimit.load();
    int w = 0;
    int h = 0;
    bool originalSize = false;
    std::vector<std::uint8_t> pixels;
    if (tryAbortIfCanceled(key))
      continue;
    if (m_provider) {
      const int providerSide = std::clamp(decodeTarget, 1, kMaxSvgRenderPx);
      const auto reserve = 8ULL * providerSide * providerSide + kMaxCacheFileBytes;
      if (!acquireMemoryBudget(key, reserve)) {
        (void)tryAbortIfCanceled(key);
        continue;
      }
      auto provided =
          m_provider(sourceFd.get(), path, sourceSize, sourceLimit, providerSide, key.diskCache, [this, &key] {
            return isCanceledOrShuttingDown(key);
          });
      releaseMemoryBudget(reserve);
      if (tryAbortIfCanceled(key))
        continue;
      if (provided
          && provided->width > 0
          && provided->height > 0
          && provided->width <= providerSide
          && provided->height <= providerSide
          && provided->pixels.size() == 4ULL * provided->width * provided->height) {
        w = provided->width;
        h = provided->height;
        pixels = std::move(provided->pixels);
      }
    }
    if (pixels.empty()) {
      const bool svgExt = DirectoryScanner::isSvgPath(path);
      const std::size_t maxSourceForPath = svgExt ? std::min(kMaxSvgSourceBytes, sourceLimit) : sourceLimit;
      if (sourceSize > maxSourceForPath) {
        kLog.warn("rejecting oversized thumbnail source {} ({} bytes > {} limit)", path, sourceSize, maxSourceForPath);
        finishWithoutSourceImage();
        continue;
      }

      if (!acquireMemoryBudget(key, sourceSize)) {
        (void)tryAbortIfCanceled(key);
        continue;
      }

      struct MemoryLease {
        ThumbnailService* service = nullptr;
        std::size_t bytes = 0;
        void release() {
          if (service != nullptr) {
            service->releaseMemoryBudget(bytes);
            service = nullptr;
            bytes = 0;
          }
        }
        ~MemoryLease() { release(); }
      } lease{this, sourceSize};

      if (tryAbortIfCanceled(key)) {
        continue;
      }

      auto bytes = readBoundedFd(sourceFd.get(), maxSourceForPath, sourceSize);
      sourceFd.reset();
      if (bytes.empty()) {
        lease.release();
        finishWithoutSourceImage();
        continue;
      }

      if (tryAbortIfCanceled(key)) {
        continue;
      }

      const bool svgLike = svg_rasterizer::looksLikeSvg(bytes);
      if (svgExt || svgLike) {
        const auto prepared = svg_rasterizer::inflateIfCompressed(bytes);
        if (!prepared || bytes.size() > kMaxSvgSourceBytes || (!svgLike && !svg_rasterizer::looksLikeSvg(bytes))) {
          kLog.warn(
              "rejecting invalid SVG thumbnail source {}: {}", path,
              prepared ? "payload is not SVG or exceeds the size limit" : prepared.error()
          );
          lease.release();
          finishWithoutSourceImage();
          continue;
        }

        const int clampedTargetPx = std::clamp(decodeTarget > 0 ? decodeTarget : kDefaultTargetPx, 1, kMaxSvgRenderPx);
        const std::uint64_t surfaceBytes =
            static_cast<std::uint64_t>(clampedTargetPx) * static_cast<std::uint64_t>(clampedTargetPx) * 4ULL;
        const std::uint64_t workingBytes = surfaceBytes * 2ULL;
        const std::uint64_t totalEstimate = workingBytes <= (std::numeric_limits<std::uint64_t>::max() - bytes.size())
            ? static_cast<std::uint64_t>(bytes.size()) + workingBytes
            : std::numeric_limits<std::uint64_t>::max();
        const std::size_t decodeReserveBytes =
            static_cast<std::size_t>(std::min<std::uint64_t>(totalEstimate, std::numeric_limits<std::size_t>::max()));

        if (decodeReserveBytes > lease.bytes) {
          lease.release();
          if (!acquireMemoryBudget(key, decodeReserveBytes)) {
            (void)tryAbortIfCanceled(key);
            continue;
          }
          lease.service = this;
          lease.bytes = decodeReserveBytes;
        }

        {
          std::function<void(const std::string&, std::size_t)> observer;
          std::size_t usedNow = 0;
          {
            std::scoped_lock lock(g_testHookMutex);
            observer = g_decodeObserverHook;
          }
          if (observer) {
            {
              std::scoped_lock lock(m_memoryMutex);
              usedNow = m_usedMemoryBytes;
            }
            observer(path, usedNow);
          }
        }

        if (tryAbortIfCanceled(key)) {
          continue;
        }

        auto rendered = svg_rasterizer::rasterize(bytes, clampedTargetPx, svg_rasterizer::Sizing::Thumbnail);
        bytes.clear();
        bytes.shrink_to_fit();
        if (!rendered) {
          kLog.warn("failed to rasterize SVG thumbnail {}: {}", path, rendered.error());
          lease.release();
          finishWithoutSourceImage();
          continue;
        }

        w = rendered->width;
        h = rendered->height;
        pixels = std::move(rendered->pixels);
        lease.release();
      } else {
        const RasterDecodeLimits decodeLimits{
            .maxDimension = kMaxImageDimension,
            .maxDecodedBytes = kMaxDecodedBytes,
        };
        std::size_t decodeReserveBytes = bytes.size();
        if (const auto extent = probeRasterImageExtent(bytes.data(), bytes.size())) {
          const std::uint64_t decodedBytes =
              static_cast<std::uint64_t>(extent->width) * static_cast<std::uint64_t>(extent->height) * 4ULL;
          if (extent->width <= 0
              || extent->height <= 0
              || extent->width > decodeLimits.maxDimension
              || extent->height > decodeLimits.maxDimension
              || decodedBytes > decodeLimits.maxDecodedBytes) {
            kLog.warn(
                "rejecting oversized thumbnail source {} ({}x{}, {} bytes decoded)", path, extent->width,
                extent->height, decodedBytes
            );
            lease.release();
            finishWithoutSourceImage();
            continue;
          }
          // Account for compressed input + decoded RGBA pixels + decoder/resize working buffer.
          const std::uint64_t workingBytes = decodedBytes <= (std::numeric_limits<std::uint64_t>::max() / 2ULL)
              ? decodedBytes * 2ULL
              : std::numeric_limits<std::uint64_t>::max();
          const std::uint64_t totalEstimate = workingBytes <= (std::numeric_limits<std::uint64_t>::max() - bytes.size())
              ? static_cast<std::uint64_t>(bytes.size()) + workingBytes
              : std::numeric_limits<std::uint64_t>::max();
          decodeReserveBytes =
              static_cast<std::size_t>(std::min<std::uint64_t>(totalEstimate, std::numeric_limits<std::size_t>::max()));
        }

        if (decodeReserveBytes > lease.bytes) {
          // Release the compressed-input lease before acquiring the combined decode
          // reservation to avoid hold-and-wait deadlocks across workers.
          lease.release();
          if (!acquireMemoryBudget(key, decodeReserveBytes)) {
            (void)tryAbortIfCanceled(key);
            continue;
          }
          lease.service = this;
          lease.bytes = decodeReserveBytes;
        }

        {
          std::function<void(const std::string&, std::size_t)> observer;
          std::size_t usedNow = 0;
          {
            std::scoped_lock lock(g_testHookMutex);
            observer = g_decodeObserverHook;
          }
          if (observer) {
            {
              std::scoped_lock lock(m_memoryMutex);
              usedNow = m_usedMemoryBytes;
            }
            observer(path, usedNow);
          }
        }

        if (tryAbortIfCanceled(key)) {
          continue;
        }

        auto decoded = decodeRasterImage(bytes.data(), bytes.size(), decodeLimits);
        bytes.clear();
        bytes.shrink_to_fit();
        if (!decoded) {
          lease.release();
          finishWithoutSourceImage();
          continue;
        }

        w = decoded->width;
        h = decoded->height;
        originalSize = std::max(w, h) <= decodeTarget;
        pixels = std::move(decoded->pixels);

        if (!resizeThumbnail(pixels, w, h, decodeTarget)) {
          lease.release();
          finishWithoutSourceImage();
          continue;
        }

        lease.release();
      }
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    if (OwnedFd cacheDir = webpCache ? openPrivateCacheDir(true) : OwnedFd{}; cacheDir) {
      const std::string leaf = cacheLeafForStat(path, openedStat, key.targetPx);
      std::uint8_t* encoded = nullptr;
      const std::size_t encodedSize = WebPEncodeRGBA(pixels.data(), w, h, w * 4, kThumbnailWebPQuality, &encoded);
      if (encoded != nullptr && encodedSize > 0) {
        (void)writeCacheEntryAtomic(cacheDir.get(), leaf, encoded, encodedSize);
        WebPFree(encoded);
      }
    }

    if (tryAbortIfCanceled(key)) {
      continue;
    }

    if (m_cacheBackend == CacheBackend::FreedesktopPng && !sourceStillCurrent(path, openedStat)) {
      result.failed = true;
      pushResult(std::move(result));
      continue;
    }

    if (pngIdentity) {
      auto encoded =
          freedesktop_thumbnail::encode(pixels.data(), w, h, *pngIdentity, m_cacheSoftware, pngSize, originalSize);
      if (!encoded.empty()
          && encoded.size() <= kMaxCacheFileBytes
          && !isCanceledOrShuttingDown(key)
          && sourceStillCurrent(path, openedStat)) {
        if (const auto directory = openPrivateCacheDir(true, pngSize);
            directory && writeCacheEntryAtomic(directory.get(), pngIdentity->leaf, encoded.data(), encoded.size()))
          scheduleCachePrune();
      }
      if (!resizeThumbnail(pixels, w, h, key.targetPx)) {
        result.failed = true;
        pushResult(std::move(result));
        continue;
      }
    }
    if (tryAbortIfCanceled(key))
      continue;

    result.rgba = std::move(pixels);
    result.width = w;
    result.height = h;
    pushResult(std::move(result));
  }
}

void setThumbnailCacheWriteHookForTesting(std::function<bool(const std::string&, const std::string&)> hook);
void setThumbnailDecodeObserverHookForTesting(std::function<void(const std::string&, std::size_t)> hook);

void setThumbnailCacheWriteHookForTesting(std::function<bool(const std::string&, const std::string&)> hook) {
  std::scoped_lock lock(g_testHookMutex);
  g_cacheWriteTestHook = std::move(hook);
}

void setThumbnailDecodeObserverHookForTesting(std::function<void(const std::string&, std::size_t)> hook) {
  std::scoped_lock lock(g_testHookMutex);
  g_decodeObserverHook = std::move(hook);
}
