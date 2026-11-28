#include "config/atomic_file.h"
#include "config/config_monitor.h"
#include "config/files_config_validator.h"
#include "test_isolation.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  void writeFile(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out << content);
  }

  void dispatchOnce(PollSource& source) {
    std::vector<pollfd> fds;
    const auto idx = source.addPollFds(fds);
    TEST_CHECK(!fds.empty());
    TEST_CHECK(::poll(fds.data(), fds.size(), 1000) == 1);
    source.dispatch(fds, idx);
  }

  void testMonitorLifecycleIncludesAndSuppression() {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-config-monitor"});
    const fs::path configDir = env.config() / "nythera" / "files";
    const fs::path extraFile = env.work() / "extra.toml";

    nythera::config::ApplicationConfigMonitorRegistry registry;
    auto& monitor = registry.addMonitor({
        .appId = "files",
        .displayName = "Nythera Files",
        .configDir = configDir,
        .validator = nythera::config::validateFilesConfiguration,
    });
    TEST_CHECK(registry.isMonitoring("files"));
    TEST_CHECK(!registry.isMonitoring("unknown"));

    int problems = 0;
    int manualReloads = 0;
    int silentReloads = 0;
    nythera::config::ConfigurationProblem lastProblem;
    monitor.setProblemCallback([&](const auto& problem, const fs::path& dir) {
      ++problems;
      lastProblem = problem;
      const auto notice = nythera::config::invalidConfigurationNotice(problem, dir);
      TEST_CHECK(notice.isError);
      TEST_CHECK(notice.title == "Invalid configuration");
      TEST_CHECK(notice.osdIcon == "alert-triangle");
      TEST_CHECK(notice.desktopIcon == "dialog-warning");
      TEST_CHECK(notice.osdHideDelayMs == 5000);
    });
    monitor.setReloadCallback([&](const auto&, bool manual) {
      if (manual) {
        ++manualReloads;
      } else {
        ++silentReloads;
      }
    });

    // Creating an empty config directory attaches watches silently without a reload alert.
    fs::create_directories(configDir);
    dispatchOnce(registry);
    TEST_CHECK(problems == 0 && manualReloads == 0);

    // Writing an included external file and root config emits manual reload and watches includes.
    writeFile(extraFile, "[files]\nshow_hidden = true\n");
    TEST_CHECK(writeTextFileAtomic(configDir / "config.toml", "[include]\nfiles = ['$HOME/../work/extra.toml']\n"));
    dispatchOnce(registry);
    TEST_CHECK(problems == 0 && manualReloads == 1);

    // Invalid syntax in an included file emits one error and suppresses duplicate reloads.
    writeFile(extraFile, "[files]\nshow_hidden = true g\n");
    dispatchOnce(registry);
    TEST_CHECK(problems == 1 && manualReloads == 1);
    const auto notice = nythera::config::invalidConfigurationNotice(lastProblem, configDir);
    TEST_CHECK(notice.body.contains("extra.toml · Line 2, column "));
    TEST_CHECK(notice.body.contains("> Unexpected 'g'; expected comment or line break"));
    TEST_CHECK(!monitor.reload(true));
    TEST_CHECK(problems == 1);

    // Recovering to valid config emits Config reloaded and resets duplicate tracking.
    writeFile(extraFile, "[files]\nshow_hidden = true\n");
    dispatchOnce(registry);
    TEST_CHECK(problems == 1 && manualReloads == 2);
    const auto reloadedNotice = nythera::config::reloadedConfigurationNotice();
    TEST_CHECK(!reloadedNotice.isError);
    TEST_CHECK(reloadedNotice.title == "Config reloaded");
    TEST_CHECK(reloadedNotice.osdIcon == "circle-check");
    TEST_CHECK(reloadedNotice.desktopIcon == "dialog-information");
    TEST_CHECK(reloadedNotice.osdHideDelayMs == 1000);

    // Same invalid schema error after recovery re-alerts.
    writeFile(extraFile, "[files]\nview_scale = 99.0\n");
    dispatchOnce(registry);
    TEST_CHECK(problems == 2);
    TEST_CHECK(lastProblem.message == "Invalid Files setting 'files.view_scale'");

    writeFile(extraFile, "[files]\nshow_hidden = false\n");
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == 3);

    // Cross-process self-write marker suppresses Files-authored atomic writes.
    const fs::path rootConfig = configDir / "config.toml";
    const fs::path filesTheme = configDir / "theme.toml";
    const std::string rootContent = "[include]\nfiles = ['$HOME/../work/extra.toml']\n\n[files]\ngrid_view = true\n";
    const std::string themeContent = "[theme]\ntransparency_mode = \"soft\"\n";
    const nythera::config::RecordedConfigWrite batch[]{
        {rootConfig, rootContent},
        {filesTheme, themeContent},
    };
    TEST_CHECK(nythera::config::recordApplicationConfigWrites("files", batch));
    TEST_CHECK(writeTextFileAtomic(rootConfig, rootContent));
    TEST_CHECK(writeTextFileAtomic(filesTheme, themeContent));
    const int beforeManual = manualReloads;
    const int beforeSilent = silentReloads;
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == beforeManual);
    TEST_CHECK(silentReloads == beforeSilent + 1);

    // Re-writing the same file externally without a new marker token IS announced (token consumed).
    TEST_CHECK(writeTextFileAtomic(rootConfig, rootContent));
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == beforeManual + 1);
  }

  void testStartupProblemAndUnsupportedSection() {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-config-monitor-startup"});
    const fs::path configDir = env.config() / "nythera" / "files";
    writeFile(configDir / "config.toml", "[other]\nfoo = 1\n");

    nythera::config::ApplicationConfigMonitor monitor({
        .appId = "files",
        .displayName = "Nythera Files",
        .configDir = configDir,
        .validator = nythera::config::validateFilesConfiguration,
    });
    TEST_CHECK(monitor.activeProblem().has_value());
    int reported = 0;
    monitor.setProblemCallback([&](const auto& problem, const fs::path&) {
      ++reported;
      TEST_CHECK(problem.message == "Invalid Files section 'other'");
    });
    TEST_CHECK(reported == 1);
  }

  void testSelfWriteMarkerLifecycle() {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-config-monitor-marker"});
    const fs::path configDir = env.config() / "nythera" / "files";
    const fs::path rootConfig = configDir / "config.toml";
    writeFile(rootConfig, "[files]\ngrid_view = false\n");

    nythera::config::ApplicationConfigMonitorRegistry registry;
    auto& monitor = registry.addMonitor({
        .appId = "files",
        .displayName = "Nythera Files",
        .configDir = configDir,
        .validator = nythera::config::validateFilesConfiguration,
    });
    int manualReloads = 0;
    monitor.setReloadCallback([&](const auto&, bool manual) { manualReloads += manual ? 1 : 0; });

    const std::string recorded = "[files]\ngrid_view = true\n";
    const nythera::config::RecordedConfigWrite batch[]{{rootConfig, recorded}};
    const auto markerPath = nythera::config::applicationConfigWriteMarkerPath("files");

    // A writer killed after recording but before writing leaves a marker that must not hide other edits.
    TEST_CHECK(nythera::config::recordApplicationConfigWrites("files", batch));
    TEST_CHECK(fs::exists(markerPath));
    TEST_CHECK(writeTextFileAtomic(rootConfig, "[files]\ngrid_view = false\nshow_hidden = true\n"));
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == 1);

    // A marker left behind by a crashed writer expires, even when the content later matches it.
    TEST_CHECK(nythera::config::recordApplicationConfigWrites("files", batch));
    auto marker = nlohmann::json::parse(std::ifstream(markerPath));
    marker["timestamp_ms"] = marker["timestamp_ms"].get<std::int64_t>() - 60'000;
    writeFile(markerPath, marker.dump());
    TEST_CHECK(writeTextFileAtomic(rootConfig, recorded));
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == 2);

    // A fresh marker with matching content is still suppressed, and clearing it removes the file.
    TEST_CHECK(nythera::config::recordApplicationConfigWrites("files", batch));
    TEST_CHECK(writeTextFileAtomic(rootConfig, "[files]\ngrid_view = false\n"));
    nythera::config::clearApplicationConfigWrites("files");
    TEST_CHECK(!fs::exists(markerPath));
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == 3);
    nythera::config::clearApplicationConfigWrites("files"); // Idempotent when already absent.
  }

  void testAdditionalWatchFilesAndSelfWriteSuppression() {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-config-monitor-extra"});
    const fs::path configDir = env.config() / "nythera" / "files";
    const fs::path rootConfig = configDir / "config.toml";
    const fs::path searchConfig = configDir / "search" / "config.toml";
    writeFile(rootConfig, "[files]\ngrid_view = false\n");
    writeFile(searchConfig, "[options]\nshow_hidden = false\n");

    nythera::config::ApplicationConfigMonitorRegistry registry;
    auto& monitor = registry.addMonitor({
        .appId = "files",
        .displayName = "Nythera Files",
        .configDir = configDir,
        .validator = nythera::config::validateFilesConfiguration,
        .additionalWatchFiles = {searchConfig},
    });
    int manualReloads = 0;
    int silentReloads = 0;
    monitor.setReloadCallback([&](const auto&, bool manual) {
      if (manual) {
        ++manualReloads;
      } else {
        ++silentReloads;
      }
    });

    // External modification of an additional watched file triggers a manual reload.
    TEST_CHECK(writeTextFileAtomic(searchConfig, "[options]\nshow_hidden = true\n"));
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == 1);
    TEST_CHECK(silentReloads == 0);

    // In-process self-write of an additional watched file suppresses manual reload notification.
    const std::string selfContent = "[options]\nshow_hidden = false\n";
    monitor.recordInProcessSelfWrite(searchConfig, selfContent);
    TEST_CHECK(writeTextFileAtomic(searchConfig, selfContent));
    dispatchOnce(registry);
    TEST_CHECK(manualReloads == 1);
    TEST_CHECK(silentReloads == 1);
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    testMonitorLifecycleIncludesAndSuppression();
    testStartupProblemAndUnsupportedSection();
    testSelfWriteMarkerLifecycle();
    testAdditionalWatchFilesAndSelfWriteSuppression();
  });
}
