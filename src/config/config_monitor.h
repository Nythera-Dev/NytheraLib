#pragma once

#include "app/poll_source.h"
#include "config/config_merge.h"
#include "config/schema/diagnostics.h"
#include "core/inotify/inotify.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nythera::config {

  struct ConfigurationProblem {
    schema::SourceOrigin origin;
    std::string message;

    bool operator==(const ConfigurationProblem&) const = default;
  };

  struct ConfigurationNotice {
    std::string title;
    std::string osdIcon;
    std::string desktopIcon;
    int osdHideDelayMs = 0;
    std::string body;
    bool isError = false;
  };

  struct RecordedConfigWrite {
    std::filesystem::path path;
    std::string content;
  };

  // Formats raw toml++/schema parse diagnostics into concise user-facing text.
  [[nodiscard]] std::string displayConfigurationDiagnostic(std::string_view message);

  // Formats the origin directory, filename, line/column, and diagnostic body.
  [[nodiscard]] std::string
  formatConfigurationProblemBody(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory);

  // Builds the standardized invalid-configuration alert notice (5s warning OSD/notification).
  [[nodiscard]] ConfigurationNotice
  invalidConfigurationNotice(const ConfigurationProblem& problem, const std::filesystem::path& configDirectory);

  // Builds the standardized reload confirmation notice (1s confirmation OSD/notification).
  [[nodiscard]] ConfigurationNotice reloadedConfigurationNotice();

  // Cross-process marker file used so an application writing its own config does not
  // cause an external monitor (e.g. Nythera Shell) to announce a manual reload.
  [[nodiscard]] std::filesystem::path applicationConfigWriteMarkerPath(std::string_view appId);
  bool recordApplicationConfigWrites(std::string_view appId, std::span<const RecordedConfigWrite> writes);
  void clearApplicationConfigWrites(std::string_view appId);

  // Watches and validates an application's configuration directory (including included files
  // and symlinks) using NytheraLib's merge + inotify infrastructure.
  class ApplicationConfigMonitor final : public PollSource {
  public:
    using Validator = std::function<std::optional<ConfigurationProblem>(const SourcedMergeResult&)>;
    using ProblemCallback = std::function<void(const ConfigurationProblem&, const std::filesystem::path&)>;
    using ReloadCallback = std::function<void(const SourcedMergeResult&, bool manualReload)>;

    struct Options {
      std::string appId;
      std::string displayName;
      std::filesystem::path configDir;
      Validator validator;
    };

    explicit ApplicationConfigMonitor(Options options);

    [[nodiscard]] const std::string& appId() const noexcept { return m_appId; }
    [[nodiscard]] const std::string& displayName() const noexcept { return m_displayName; }
    [[nodiscard]] const std::filesystem::path& configDir() const noexcept { return m_configDir; }
    [[nodiscard]] const std::optional<ConfigurationProblem>& activeProblem() const noexcept { return m_problem; }

    void setProblemCallback(ProblemCallback callback);
    void setReloadCallback(ReloadCallback callback);
    void recordInProcessSelfWrite(const std::filesystem::path& path, std::string content);
    bool reload(bool notifyReload = true);
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    void reportProblem(ConfigurationProblem problem);
    void updateWatches(const SourcedMergeResult& loaded);

    struct Watch {
      std::filesystem::path directory;
      std::set<std::string> names;
      bool scanToml = false;
    };

    std::string m_appId;
    std::string m_displayName;
    std::filesystem::path m_configDir;
    Validator m_validator;
    ReloadCallback m_onReload;
    ProblemCallback m_onProblem;
    std::optional<ConfigurationProblem> m_problem;
    Inotify m_inotify;
    std::map<int, Watch> m_watches;
    std::map<std::filesystem::path, std::string> m_selfWrites;
    std::map<std::filesystem::path, std::string> m_consumedFileTokens;
  };

  // Multiplexes multiple ApplicationConfigMonitor instances as a single PollSource.
  class ApplicationConfigMonitorRegistry final : public PollSource {
  public:
    ApplicationConfigMonitor& addMonitor(ApplicationConfigMonitor::Options options);
    [[nodiscard]] bool isMonitoring(std::string_view appId) const;
    [[nodiscard]] ApplicationConfigMonitor* findMonitor(std::string_view appId) const;
    [[nodiscard]] const std::vector<std::unique_ptr<ApplicationConfigMonitor>>& monitors() const noexcept {
      return m_monitors;
    }

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    std::vector<std::unique_ptr<ApplicationConfigMonitor>> m_monitors;
    std::vector<std::size_t> m_lastFdCounts;
  };

} // namespace nythera::config
