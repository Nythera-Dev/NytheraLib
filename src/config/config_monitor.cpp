#include "config/config_monitor.h"

#include "config/atomic_file.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <glib.h>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace nythera::config {

  namespace {
    namespace fs = std::filesystem;

    constexpr Logger kLog("app-config-monitor");
    constexpr std::uintmax_t kMaxMarkerBytes = 64ULL * 1024ULL;
    constexpr std::int64_t kMaxSelfWriteAgeMs = 10'000;

    fs::path canonicalPath(const fs::path& path) {
      std::error_code ec;
      auto resolved = fs::weakly_canonical(path, ec);
      return ec ? path.lexically_normal() : resolved;
    }

    std::string sanitizeAppId(std::string_view appId) {
      std::string out;
      out.reserve(appId.size());
      for (const char ch : appId) {
        if ((ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '-'
            || ch == '_') {
          out.push_back(ch);
        }
      }
      return out.empty() ? std::string("app") : out;
    }

    std::string sha256Hex(std::string_view data) {
      GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
      if (checksum == nullptr) {
        return {};
      }
      if (!data.empty()) {
        g_checksum_update(checksum, reinterpret_cast<const guchar*>(data.data()), static_cast<gssize>(data.size()));
      }
      std::string digest;
      if (const gchar* hex = g_checksum_get_string(checksum); hex != nullptr) {
        digest = hex;
      }
      g_checksum_free(checksum);
      return digest;
    }

    std::optional<std::string> readFileBytes(const fs::path& path, std::uintmax_t maxBytes) {
      std::error_code ec;
      const auto status = fs::symlink_status(path, ec);
      if (ec || status.type() != fs::file_type::regular) {
        return std::nullopt;
      }
      const auto size = fs::file_size(path, ec);
      if (ec || size > maxBytes) {
        return std::nullopt;
      }
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::string content(static_cast<std::size_t>(size), '\0');
      if (size > 0 && !in.read(content.data(), static_cast<std::streamsize>(size))) {
        return std::nullopt;
      }
      return content;
    }

    std::string fileSha256Hex(const fs::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return {};
      }
      GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
      if (checksum == nullptr) {
        return {};
      }
      char buffer[8192];
      while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize count = in.gcount();
        if (count > 0) {
          g_checksum_update(checksum, reinterpret_cast<const guchar*>(buffer), static_cast<gssize>(count));
        }
      }
      std::string digest;
      if (!in.bad()) {
        if (const gchar* hex = g_checksum_get_string(checksum); hex != nullptr) {
          digest = hex;
        }
      }
      g_checksum_free(checksum);
      return digest;
    }

    std::int64_t currentEpochMs() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    struct WriteMarker {
      std::string token;
      std::int64_t timestampMs = 0;
      std::map<std::string, std::string> files;
    };

    std::optional<WriteMarker> loadFreshWriteMarker(const fs::path& markerPath) {
      if (markerPath.empty()) {
        return std::nullopt;
      }
      const auto raw = readFileBytes(markerPath, kMaxMarkerBytes);
      if (!raw.has_value()) {
        return std::nullopt;
      }
      const auto json = nlohmann::json::parse(*raw, nullptr, false);
      if (json.is_discarded() || !json.is_object()) {
        return std::nullopt;
      }
      const auto tokenIt = json.find("token");
      const auto tsIt = json.find("timestamp_ms");
      const auto filesIt = json.find("files");
      if (tokenIt == json.end()
          || !tokenIt->is_string()
          || tsIt == json.end()
          || !tsIt->is_number_integer()
          || filesIt == json.end()
          || !filesIt->is_object()) {
        return std::nullopt;
      }
      const std::int64_t timestampMs = tsIt->get<std::int64_t>();
      const std::int64_t nowMs = currentEpochMs();
      if (timestampMs > nowMs + 1'000 || (nowMs - timestampMs) > kMaxSelfWriteAgeMs) {
        return std::nullopt;
      }
      WriteMarker marker{
          .token = tokenIt->get<std::string>(),
          .timestampMs = timestampMs,
      };
      if (marker.token.empty()) {
        return std::nullopt;
      }
      for (const auto& [pathKey, digestVal] : filesIt->items()) {
        if (digestVal.is_string()) {
          marker.files.emplace(pathKey, digestVal.get<std::string>());
        }
      }
      return marker;
    }
  } // namespace

  std::string displayConfigurationDiagnostic(std::string_view message) {
    constexpr std::string_view prefix = "Error while parsing ";
    constexpr std::string_view separator = ": ";
    if (!message.starts_with(prefix)) {
      return std::string(message);
    }
    const auto scopeEnd = message.find(separator, prefix.size());
    if (scopeEnd == std::string_view::npos || scopeEnd + separator.size() == message.size()) {
      return std::string(message);
    }
    const auto scope = message.substr(prefix.size(), scopeEnd - prefix.size());
    const auto reason = message.substr(scopeEnd + separator.size());

    constexpr std::string_view expectedPrefix = "expected ";
    constexpr std::string_view sawSeparator = ", saw ";
    if (reason.starts_with(expectedPrefix)) {
      const auto saw = reason.find(sawSeparator, expectedPrefix.size());
      if (saw != std::string_view::npos && saw + sawSeparator.size() < reason.size()) {
        auto expected = reason.substr(expectedPrefix.size(), saw - expectedPrefix.size());
        const auto token = reason.substr(saw + sawSeparator.size());
        if (expected.starts_with("a ")) {
          expected.remove_prefix(2);
        } else if (expected.starts_with("an ")) {
          expected.remove_prefix(3);
        }
        return "Unexpected "
            + std::string(token == "end-of-file" ? "end of file" : token)
            + "; expected "
            + std::string(expected);
      }
    }
    if (reason == "encountered end-of-file") {
      if (scope == "string" || scope == "literal string") {
        return "Unterminated " + std::string(scope);
      }
      return "Unexpected end of file in " + std::string(scope);
    }
    if (reason.starts_with("exceeds length limit")) {
      return "Value " + std::string(reason) + " for " + std::string(scope);
    }

    std::string concise(reason);
    if (!concise.empty() && concise.front() >= 'a' && concise.front() <= 'z') {
      concise.front() = static_cast<char>(concise.front() - 'a' + 'A');
    }
    return concise;
  }

  std::string
  formatConfigurationProblemBody(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory) {
    const fs::path file(problem.origin.file);
    const auto parent = file.empty() ? configDirectory : file.parent_path();
    std::string body = FileUtils::xdgPathForDisplay(parent.string());
    if (!body.empty()) {
      if (!body.ends_with('/')) {
        body += '/';
      }
      body += '\n';
    }
    if (!file.empty()) {
      body += file.filename().string();
      if (problem.origin.line != 0) {
        body += " · Line " + std::to_string(problem.origin.line);
        if (problem.origin.column != 0) {
          body += ", column " + std::to_string(problem.origin.column);
        }
      }
      body += '\n';
    }
    return body + "> " + displayConfigurationDiagnostic(problem.message);
  }

  ConfigurationNotice
  invalidConfigurationNotice(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory) {
    const std::string formatted = formatConfigurationProblemBody(problem, configDirectory);
    gchar* valid = g_utf8_make_valid(formatted.data(), static_cast<gssize>(formatted.size()));
    std::string body = valid != nullptr ? std::string(valid) : std::string{};
    g_free(valid);
    return {
        .title = "Invalid configuration",
        .osdIcon = "alert-triangle",
        .desktopIcon = "dialog-warning",
        .osdHideDelayMs = 5000,
        .body = std::move(body),
        .isError = true,
    };
  }

  ConfigurationNotice reloadedConfigurationNotice() {
    return {
        .title = "Config reloaded",
        .osdIcon = "circle-check",
        .desktopIcon = "dialog-information",
        .osdHideDelayMs = 1000,
        .body = {},
        .isError = false,
    };
  }

  std::filesystem::path applicationConfigWriteMarkerPath(std::string_view appId) {
    fs::path runtimeDir;
    if (const char* xdgRuntime = std::getenv("XDG_RUNTIME_DIR"); xdgRuntime != nullptr && xdgRuntime[0] != '\0') {
      runtimeDir = fs::path(xdgRuntime) / "nythera-config-writes";
    } else {
      runtimeDir = fs::temp_directory_path() / ("nythera-config-writes-" + std::to_string(::geteuid()));
    }
    return runtimeDir / (sanitizeAppId(appId) + ".json");
  }

  bool recordApplicationConfigWrites(std::string_view appId, std::span<const RecordedConfigWrite> writes) {
    if (writes.empty()) {
      return true;
    }
    const fs::path markerPath = applicationConfigWriteMarkerPath(appId);
    if (markerPath.empty()) {
      return false;
    }

    std::error_code ec;
    if (!FileUtils::createPrivateDirectories(markerPath.parent_path(), ec)) {
      return false;
    }

    static std::atomic<std::uint64_t> sequence{0};
    const auto tokenNum = sequence.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t nowMs = currentEpochMs();
    const std::string token = std::to_string(::getpid()) + "-" + std::to_string(tokenNum) + "-" + std::to_string(nowMs);

    nlohmann::json files = nlohmann::json::object();
    for (const auto& entry : writes) {
      if (entry.path.empty()) {
        continue;
      }
      const std::string digest = sha256Hex(entry.content);
      if (digest.empty()) {
        return false;
      }
      files[canonicalPath(entry.path).string()] = digest;
      files[entry.path.lexically_normal().string()] = digest;
    }

    const nlohmann::json root{
        {"token", token},
        {"timestamp_ms", nowMs},
        {"files", std::move(files)},
    };
    return writeTextFileAtomic(markerPath, root.dump(), FileUtils::privateFileMode());
  }

  void clearApplicationConfigWrites(std::string_view appId) {
    const fs::path markerPath = applicationConfigWriteMarkerPath(appId);
    if (markerPath.empty()) {
      return;
    }
    std::error_code ec;
    fs::remove(markerPath, ec);
  }

  ApplicationConfigMonitor::ApplicationConfigMonitor(Options options)
      : m_appId(std::move(options.appId)), m_displayName(std::move(options.displayName)),
        m_configDir(options.configDir.empty() ? fs::path{} : fs::absolute(options.configDir)),
        m_validator(std::move(options.validator)) {
    reload(/*notifyReload=*/false);
  }

  void ApplicationConfigMonitor::setProblemCallback(ProblemCallback callback) {
    m_onProblem = std::move(callback);
    if (m_problem && m_onProblem) {
      m_onProblem(*m_problem, m_configDir);
    }
  }

  void ApplicationConfigMonitor::setReloadCallback(ReloadCallback callback) { m_onReload = std::move(callback); }

  void ApplicationConfigMonitor::recordInProcessSelfWrite(const std::filesystem::path& path, std::string content) {
    if (!path.empty()) {
      m_selfWrites.insert_or_assign(path, std::move(content));
    }
  }

  void ApplicationConfigMonitor::reportProblem(ConfigurationProblem problem) {
    if (m_problem == problem) {
      return;
    }
    m_problem = std::move(problem);
    if (m_onProblem) {
      m_onProblem(*m_problem, m_configDir);
    }
  }

  void ApplicationConfigMonitor::updateWatches(const SourcedMergeResult& loaded) {
    if (m_configDir.empty()) {
      return;
    }

    std::map<fs::path, Watch> desired;
    auto add = [&](fs::path path, bool contents) {
      std::error_code error;
      fs::path name;
      if (!contents) {
        name = path.filename();
        path = path.parent_path();
      }
      while (!path.empty() && !fs::is_directory(path, error)) {
        name = path.filename();
        const fs::path parent = path.parent_path();
        if (parent == path) {
          return;
        }
        path = parent;
      }
      if (path.empty()) {
        return;
      }
      const fs::path resolved = canonicalPath(path);
      auto& watch = desired[resolved];
      watch.directory = resolved;
      if (name.empty()) {
        watch.scanToml = true;
      } else {
        watch.names.insert(name.string());
      }
    };

    add(m_configDir, true);
    add(m_configDir, false);
    for (const auto& dir : loaded.config.includeDirs) {
      add(dir, true);
    }
    for (const auto& path : loaded.watchPaths) {
      add(path, false);
      const fs::path target = canonicalPath(path);
      if (target != path) {
        add(target, false);
      }
      // weakly_canonical cannot follow a dangling link. Watch its named target
      // so recreating an external included file also recovers the configuration.
      std::error_code error;
      const fs::path link = fs::read_symlink(path, error);
      if (!error) {
        add(link.is_absolute() ? link : path.parent_path() / link, false);
      }
    }
    for (const auto& path : loaded.config.loadedFiles) {
      add(path, false);
    }

    constexpr auto mask = IN_CLOSE_WRITE
        | IN_MOVED_TO
        | IN_MOVED_FROM
        | IN_DELETE
        | IN_CREATE
        | IN_DELETE_SELF
        | IN_MOVE_SELF
        | IN_ATTRIB;
    std::map<int, Watch> watches;
    for (auto& [path, watch] : desired) {
      if (auto descriptor = m_inotify.watch(path, mask)) {
        watches.emplace(*descriptor, std::move(watch));
      }
    }
    for (const auto& [descriptor, watch] : m_watches) {
      if (!watches.contains(descriptor)) {
        m_inotify.unwatch(descriptor);
      }
    }
    m_watches = std::move(watches);
  }

  void ApplicationConfigMonitor::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_inotify.fd() >= 0) {
      fds.push_back({.fd = m_inotify.fd(), .events = POLLIN});
    }
  }

  void ApplicationConfigMonitor::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (m_inotify.fd() < 0 || startIdx >= fds.size() || (fds[startIdx].revents & POLLIN) == 0) {
      return;
    }

    bool sawOverflow = false;
    bool sawRelevantChange = false;
    bool sawNonDirCreateEvent = false;
    bool allChangesSelfWrites = true;
    std::set<fs::path> candidateSelfWritePaths;

    m_inotify.drain([&](const inotify_event* event) {
      if ((event->mask & IN_Q_OVERFLOW) != 0) {
        sawOverflow = true;
        sawRelevantChange = true;
        sawNonDirCreateEvent = true;
        allChangesSelfWrites = false;
        return;
      }
      const auto watch = m_watches.find(event->wd);
      if (watch == m_watches.end()) {
        return;
      }
      if (event->len == 0) {
        sawRelevantChange = true;
        sawNonDirCreateEvent = true;
        allChangesSelfWrites = false;
        return;
      }
      const std::string_view name(event->name);
      if (!watch->second.names.contains(std::string(name)) && !(watch->second.scanToml && name.ends_with(".toml"))) {
        return;
      }
      if ((event->mask & IN_CREATE) != 0 && (event->mask & IN_ISDIR) != 0) {
        // Creating the watched config directory should attach watches without firing a reload alert.
        sawRelevantChange = true;
        return;
      }
      // An ordinary new regular file is read after its writer closes it.
      if ((event->mask & IN_CREATE) != 0 && (event->mask & IN_ISDIR) == 0) {
        std::error_code ec;
        if (!fs::is_symlink(watch->second.directory / name, ec)) {
          return;
        }
      }

      sawRelevantChange = true;
      sawNonDirCreateEvent = true;
      const fs::path fullPath = watch->second.directory / name;
      const bool writeOrMoveIn = (event->mask & (IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB)) != 0;
      const bool destructive = (event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0;
      if (!writeOrMoveIn || destructive) {
        allChangesSelfWrites = false;
        return;
      }
      candidateSelfWritePaths.insert(fullPath);
    });

    if (!sawRelevantChange) {
      m_selfWrites.clear();
      return;
    }

    bool suppressedSelfWrite = false;
    if (!sawOverflow && allChangesSelfWrites && !candidateSelfWritePaths.empty()) {
      std::optional<WriteMarker> marker;
      bool markerLoaded = false;
      std::vector<std::pair<fs::path, std::string>> newlyConsumedTokens;

      bool allMatched = true;
      for (const auto& changedPath : candidateSelfWritePaths) {
        if (const auto selfIt = m_selfWrites.find(changedPath); selfIt != m_selfWrites.end()) {
          std::ifstream input(selfIt->first, std::ios::binary);
          if (input && std::string(std::istreambuf_iterator<char>(input), {}) == selfIt->second) {
            continue;
          }
        }

        if (!markerLoaded) {
          marker = loadFreshWriteMarker(applicationConfigWriteMarkerPath(m_appId));
          markerLoaded = true;
        }
        if (!marker.has_value()) {
          allMatched = false;
          break;
        }

        const fs::path canon = canonicalPath(changedPath);
        const fs::path norm = changedPath.lexically_normal();
        auto fileIt = marker->files.find(canon.string());
        if (fileIt == marker->files.end()) {
          fileIt = marker->files.find(norm.string());
        }
        if (fileIt == marker->files.end()) {
          allMatched = false;
          break;
        }

        const fs::path tokenKey = canon;
        if (const auto consumedIt = m_consumedFileTokens.find(tokenKey);
            consumedIt != m_consumedFileTokens.end() && consumedIt->second == marker->token) {
          allMatched = false;
          break;
        }

        const std::string actualDigest = fileSha256Hex(changedPath);
        if (actualDigest.empty() || actualDigest != fileIt->second) {
          allMatched = false;
          break;
        }

        newlyConsumedTokens.emplace_back(tokenKey, marker->token);
      }

      if (allMatched) {
        for (auto& [key, token] : newlyConsumedTokens) {
          m_consumedFileTokens.insert_or_assign(std::move(key), std::move(token));
        }
        suppressedSelfWrite = true;
      }
    }

    m_selfWrites.clear();

    const bool notify = sawNonDirCreateEvent && !suppressedSelfWrite;
    reload(notify);
  }

  bool ApplicationConfigMonitor::reload(bool notifyReload) {
    if (m_configDir.empty()) {
      return false;
    }

    auto loaded = mergeConfigWithSources(m_configDir.string());
    std::optional<ConfigurationProblem> error;
    if (!loaded.config.firstError.empty()) {
      error = ConfigurationProblem{loaded.config.firstErrorOrigin, loaded.config.firstError};
    } else if (m_validator) {
      error = m_validator(loaded);
    }

    if (error.has_value()) {
      updateWatches(loaded);
      kLog.warn(
          "Retaining the previous {} configuration: {}", m_displayName.empty() ? m_appId : m_displayName,
          error->origin.prefixed(error->message)
      );
      reportProblem(std::move(*error));
      return false;
    }

    updateWatches(loaded);
    m_problem.reset();
    if (m_onReload) {
      m_onReload(loaded, notifyReload);
    }
    return true;
  }

  ApplicationConfigMonitor& ApplicationConfigMonitorRegistry::addMonitor(ApplicationConfigMonitor::Options options) {
    const std::string& id = options.appId;
    for (const auto& existing : m_monitors) {
      if (existing->appId() == id) {
        return *existing;
      }
    }
    auto monitor = std::make_unique<ApplicationConfigMonitor>(std::move(options));
    auto& ref = *monitor;
    m_monitors.push_back(std::move(monitor));
    return ref;
  }

  bool ApplicationConfigMonitorRegistry::isMonitoring(std::string_view appId) const {
    return findMonitor(appId) != nullptr;
  }

  ApplicationConfigMonitor* ApplicationConfigMonitorRegistry::findMonitor(std::string_view appId) const {
    for (const auto& monitor : m_monitors) {
      if (monitor->appId() == appId) {
        return monitor.get();
      }
    }
    return nullptr;
  }

  void ApplicationConfigMonitorRegistry::doAddPollFds(std::vector<pollfd>& fds) {
    m_lastFdCounts.clear();
    m_lastFdCounts.reserve(m_monitors.size());
    for (const auto& monitor : m_monitors) {
      const std::size_t before = fds.size();
      monitor->addPollFds(fds);
      m_lastFdCounts.push_back(fds.size() - before);
    }
  }

  void ApplicationConfigMonitorRegistry::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    std::size_t cursor = startIdx;
    for (std::size_t i = 0; i < m_monitors.size(); ++i) {
      const std::size_t count = i < m_lastFdCounts.size() ? m_lastFdCounts[i] : 0;
      if (count > 0 && cursor + count <= fds.size()) {
        m_monitors[i]->dispatch(fds, cursor);
      }
      cursor += count;
    }
  }

} // namespace nythera::config
