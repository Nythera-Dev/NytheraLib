#include "config/config_service.h"

#include "config/application_paths.h"
#include "config/atomic_file.h"
#include "config/config_export.h"
#include "config/config_merge.h"
#include "config/config_migrations.h"
#include "config/config_notification_sink.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_sections.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"
#include "config/widget_config.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "i18n/i18n.h"
#include "launcher/launcher_provider.h"
#include "scripting/plugin_id.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace schema = nythera::config::schema;

namespace {

  // Completed writes, renames, creates (dotfile managers create symlinks) and removals. In-progress
  // writes (IN_MODIFY) are skipped; the debounce below covers the steps of a multi-step save.
  constexpr std::uint32_t inotifyMask =
      IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF;
  constexpr std::chrono::milliseconds kWatchReloadDebounce{150};

  constexpr Logger kLog("config");

  std::filesystem::path canonicalWatchPath(const std::filesystem::path& path) {
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : resolved;
  }

  template <typename T>
  void readConfigSection(
      const toml::table& table, T& target, const nythera::config::schema::Schema<T>& sectionSchema,
      std::string_view path, nythera::config::schema::Diagnostics& diagnostics
  ) {
    T candidate = target;
    try {
      nythera::config::schema::readInto(table, candidate, sectionSchema, path, diagnostics);
      target = std::move(candidate);
    } catch (const std::exception& e) {
      diagnostics.error(std::string(path), e.what());
      kLog.warn("{}: {}", path, e.what());
    }
  }

  constexpr std::string_view kMigrationReminderOwner = "config_migration_reminders";
  constexpr std::string_view kMigrationReminderKey = "last_notification";
  struct MigrationReminderState {
    std::int64_t epochSeconds = 0;
    std::string issueFingerprint;
  };

  std::optional<MigrationReminderState> parseMigrationReminderState(std::string_view value) {
    const std::size_t separator = value.find('\n');
    if (separator == std::string_view::npos) {
      return std::nullopt;
    }

    std::int64_t epochSeconds = 0;
    const std::string_view encodedEpoch = value.substr(0, separator);
    const auto [end, error] =
        std::from_chars(encodedEpoch.data(), encodedEpoch.data() + encodedEpoch.size(), epochSeconds);
    if (error != std::errc{} || end != encodedEpoch.data() + encodedEpoch.size() || epochSeconds < 0) {
      return std::nullopt;
    }
    return MigrationReminderState{
        .epochSeconds = epochSeconds,
        .issueFingerprint = std::string(value.substr(separator + 1)),
    };
  }

  std::int64_t currentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  std::optional<double> finiteDouble(const toml::node_view<const toml::node>& node) {
    if (auto v = node.value<double>()) {
      if (!std::isfinite(*v)) {
        return std::nullopt;
      }
      return v;
    }
    if (auto v = node.value<int64_t>()) {
      return static_cast<double>(*v);
    }
    return std::nullopt;
  }

  std::vector<std::string> readStringArray(const toml::node& node) {
    std::vector<std::string> result;
    if (const auto* arr = node.as_array()) {
      for (const auto& item : *arr) {
        if (const auto* str = item.as_string()) {
          result.push_back(str->get());
        }
      }
    }
    return result;
  }

  std::optional<std::string> componentOwnerId(std::string_view ownerPath, std::string_view prefix) {
    if (!ownerPath.starts_with(prefix) || ownerPath.size() <= prefix.size()) {
      return std::nullopt;
    }
    return std::string(ownerPath.substr(prefix.size()));
  }

  void restoreInvalidComponents(
      Config& candidate, const Config& active, const nythera::config::schema::Diagnostics& diagnostics
  ) {
    const auto restorePlacementWidget = [](std::vector<DesktopWidgetState>& candidateWidgets,
                                           const std::vector<DesktopWidgetState>& activeWidgets,
                                           const std::string& id) {
      const auto candidateWidget = std::ranges::find(candidateWidgets, id, &DesktopWidgetState::id);
      const auto activeWidget = std::ranges::find(activeWidgets, id, &DesktopWidgetState::id);
      if (activeWidget != activeWidgets.end()) {
        if (candidateWidget != candidateWidgets.end()) {
          *candidateWidget = *activeWidget;
        } else {
          candidateWidgets.push_back(*activeWidget);
        }
      } else if (candidateWidget != candidateWidgets.end()) {
        candidateWidgets.erase(candidateWidget);
      }
    };

    for (const auto& entry : diagnostics.entries) {
      if (entry.severity != nythera::config::schema::Diagnostics::Severity::Error
          || entry.recoveryScope != nythera::config::schema::Diagnostics::RecoveryScope::Component) {
        continue;
      }
      if (const auto barId = componentOwnerId(entry.ownerPath, "widget.")) {
        candidate.widgets.erase(*barId);
        if (const auto activeWidget = active.widgets.find(*barId); activeWidget != active.widgets.end()) {
          candidate.widgets.emplace(*barId, activeWidget->second);
        }
      } else if (const auto desktopId = componentOwnerId(entry.ownerPath, "desktop_widgets.widget.")) {
        restorePlacementWidget(candidate.desktopWidgets.widgets, active.desktopWidgets.widgets, *desktopId);
      } else if (const auto lockscreenId = componentOwnerId(entry.ownerPath, "lockscreen_widgets.widget.")) {
        restorePlacementWidget(candidate.lockscreenWidgets.widgets, active.lockscreenWidgets.widgets, *lockscreenId);
      }
    }
  }

  DesktopWidgetState
  readDesktopWidgetState(std::string_view id, const toml::table& widgetTable, std::string_view /*colorSection*/) {
    DesktopWidgetState widget;
    widget.id = std::string(id);
    if (auto explicitId = widgetTable["id"].value<std::string>()) {
      widget.id = *explicitId;
    }
    if (auto type = widgetTable["type"].value<std::string>()) {
      widget.type = *type;
    }
    if (auto output = widgetTable["output"].value<std::string>()) {
      widget.outputName = *output;
    }
    if (auto cx = finiteDouble(widgetTable["cx"])) {
      widget.cx = static_cast<float>(*cx);
    }
    if (auto cy = finiteDouble(widgetTable["cy"])) {
      widget.cy = static_cast<float>(*cy);
    }
    if (auto placementWidth = finiteDouble(widgetTable["placement_width"])) {
      widget.placementWidth = std::max(0.0F, static_cast<float>(*placementWidth));
    }
    if (auto placementHeight = finiteDouble(widgetTable["placement_height"])) {
      widget.placementHeight = std::max(0.0F, static_cast<float>(*placementHeight));
    }
    if (auto boxWidth = finiteDouble(widgetTable["box_width"])) {
      widget.boxWidth = std::max(0.0F, static_cast<float>(*boxWidth));
    }
    if (auto boxHeight = finiteDouble(widgetTable["box_height"])) {
      widget.boxHeight = std::max(0.0F, static_cast<float>(*boxHeight));
    }
    if (auto rotation = finiteDouble(widgetTable["rotation"])) {
      widget.rotationRad = static_cast<float>(*rotation);
    }
    if (auto flipX = widgetTable["flip_x"].value<bool>()) {
      widget.flipX = *flipX;
    }
    if (auto flipY = widgetTable["flip_y"].value<bool>()) {
      widget.flipY = *flipY;
    }
    if (auto enabled = widgetTable["enabled"].value<bool>()) {
      widget.enabled = *enabled;
    }
    if (const auto* settingsTable = widgetTable["settings"].as_table()) {
      for (const auto& [key, value] : *settingsTable) {
        if (auto parsed = nythera::config::readWidgetSettingValue(value); parsed.has_value()) {
          widget.settings.emplace(std::string(key.str()), std::move(*parsed));
        }
      }
    }
    return widget;
  }

  void parseWidgetsPlacementSection(
      const toml::table& sectionTbl, DesktopWidgetsGridState& grid, std::vector<DesktopWidgetState>& widgets,
      std::string_view colorSection
  ) {
    if (const auto* gridTable = sectionTbl["grid"].as_table()) {
      if (auto visible = (*gridTable)["visible"].value<bool>()) {
        grid.visible = *visible;
      }
      if (auto cellSize = (*gridTable)["cell_size"].value<int64_t>()) {
        grid.cellSize = std::clamp(static_cast<std::int32_t>(*cellSize), 8, 256);
      }
      if (auto majorInterval = (*gridTable)["major_interval"].value<int64_t>()) {
        grid.majorInterval = std::clamp(static_cast<std::int32_t>(*majorInterval), 1, 16);
      }
    }
    if (const auto* widgetsTable = sectionTbl["widget"].as_table()) {
      std::vector<DesktopWidgetState> parsedWidgets;
      parsedWidgets.reserve(widgetsTable->size());
      for (const auto& [idNode, widgetNode] : *widgetsTable) {
        const auto* widgetTable = widgetNode.as_table();
        if (widgetTable == nullptr) {
          continue;
        }
        DesktopWidgetState widget;
        try {
          widget = readDesktopWidgetState(idNode.str(), *widgetTable, colorSection);
        } catch (const std::exception& e) {
          kLog.warn("{}.widget.{}: {}", colorSection, idNode.str(), e.what());
          continue;
        }
        if (!widget.id.empty() && !widget.type.empty()) {
          parsedWidgets.push_back(std::move(widget));
        }
      }

      std::vector<std::string> order;
      bool orderSpecified = false;
      if (const auto* orderNode = sectionTbl.get("widget_order")) {
        order = readStringArray(*orderNode);
        orderSpecified = true;
      }

      widgets.clear();
      std::vector<bool> used(parsedWidgets.size(), false);
      for (const auto& orderedId : order) {
        for (std::size_t i = 0; i < parsedWidgets.size(); ++i) {
          if (!used[i] && parsedWidgets[i].id == orderedId) {
            used[i] = true;
            widgets.push_back(std::move(parsedWidgets[i]));
            break;
          }
        }
      }
      if (!orderSpecified) {
        for (std::size_t i = 0; i < parsedWidgets.size(); ++i) {
          if (!used[i]) {
            widgets.push_back(std::move(parsedWidgets[i]));
          }
        }
      }
    }
  }

  const std::vector<KeyChord>& keybindSet(const KeybindsConfig& keybinds, KeybindAction action) {
    switch (action) {
    case KeybindAction::Validate:
      return keybinds.validate;
    case KeybindAction::Cancel:
      return keybinds.cancel;
    case KeybindAction::Left:
      return keybinds.left;
    case KeybindAction::Right:
      return keybinds.right;
    case KeybindAction::Up:
      return keybinds.up;
    case KeybindAction::Down:
      return keybinds.down;
    case KeybindAction::TabNext:
      return keybinds.tabNext;
    case KeybindAction::TabPrevious:
      return keybinds.tabPrevious;
    case KeybindAction::Delete:
      return keybinds.deleteEntry;
    case KeybindAction::Copy:
      return keybinds.copy;
    case KeybindAction::Save:
      return keybinds.save;
    }
    return keybinds.validate;
  }

  void seedLegacyDockAllowListEnabled(toml::table& sidecar, const toml::table& baseConfig) {
    auto* dock = sidecar["dock"].as_table();
    if (dock == nullptr || (*dock)["monitors"].as_array() == nullptr || dock->contains("enabled"))
      return;
    if (baseConfig["dock"]["enabled"].value_or(false))
      dock->insert("enabled", true);
  }

} // namespace

// ── Lifecycle ────────────────────────────────────────────────────────────────

ConfigService::WallpaperBatch::WallpaperBatch(ConfigService& config) : m_config(config) {
  ++m_config.m_wallpaperBatchDepth;
}

ConfigService::WallpaperBatch::~WallpaperBatch() {
  --m_config.m_wallpaperBatchDepth;
  if (m_config.m_wallpaperBatchDepth == 0 && m_config.m_wallpaperBatchDirty) {
    m_config.m_wallpaperBatchDirty = false;
    if (m_config.m_wallpaperChangeCallback) {
      m_config.m_wallpaperChangeCallback();
    }
  }
}

ConfigService::ActiveConfigLocation ConfigService::resolveActiveConfigLocation() {
  const std::string shellDir = FileUtils::applicationConfigDir(FileUtils::Application::Shell);
  const std::string sharedTheme = FileUtils::sharedThemeConfigPath();
  const std::string legacyDir = FileUtils::configDir();
  const std::string stateDir = FileUtils::stateDir();
  const std::string legacySettings = stateDir.empty() ? std::string{} : stateDir + "/settings.toml";

  const auto hasTomlFiles = [](const std::string& dir, std::string_view excludeName = {}) {
    if (dir.empty()) {
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
      return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (entry.path().extension() != ".toml") {
        continue;
      }
      if (!excludeName.empty() && entry.path().filename() == excludeName) {
        continue;
      }
      std::error_code fileEc;
      if (entry.is_regular_file(fileEc) && !fileEc) {
        return true;
      }
    }
    return false;
  };

  if (!shellDir.empty() && hasTomlFiles(shellDir)) {
    return {
        .configDir = shellDir,
        .sharedThemePath = sharedTheme,
        .legacySettingsPath = {},
        .usingLegacy = false,
    };
  }

  std::error_code ec;
  const bool hasLegacySettings = !legacySettings.empty() && std::filesystem::exists(legacySettings, ec) && !ec;
  if ((!legacyDir.empty() && hasTomlFiles(legacyDir, "theme.toml")) || hasLegacySettings) {
    return {
        .configDir = legacyDir,
        .sharedThemePath = {},
        .legacySettingsPath = hasLegacySettings ? legacySettings : std::string{},
        .usingLegacy = true,
    };
  }

  return {
      .configDir = shellDir,
      .sharedThemePath = sharedTheme,
      .legacySettingsPath = {},
      .usingLegacy = false,
  };
}

std::string ConfigService::resolveActiveStateDir() {
  const std::string shellStateDir = FileUtils::applicationStateDir(FileUtils::Application::Shell);
  if (shellStateDir.empty()) {
    return {};
  }
  std::error_code ec;
  if (std::filesystem::exists(shellStateDir, ec) || !resolveActiveConfigLocation().usingLegacy) {
    return shellStateDir;
  }
  return FileUtils::stateDir();
}

ConfigService::ConfigService(MergedConfigValidator validator, OverrideEffectivenessChecker effectiveness)
    : ConfigService(SettingsAccess::ReadWrite, std::move(validator), std::move(effectiveness)) {}

ConfigService::ConfigService(SettingsAccess access) : ConfigService(access, {}, {}) {}

ConfigService::ConfigService(
    SettingsAccess access, MergedConfigValidator validator, OverrideEffectivenessChecker effectiveness
)
    : m_settingsAccess(access), m_validator(std::move(validator)), m_effectivenessChecker(std::move(effectiveness)) {
  if (const std::string dir = resolveActiveStateDir(); !dir.empty()) {
    if (access != SettingsAccess::InheritanceOnly) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
    }
    m_stateStore.setPath(dir + "/state.toml");
    m_setupMarkerPath = dir + "/.setup-complete";
  }

  if (access != SettingsAccess::InheritanceOnly) {
    m_stateStore.load();
  }
  loadAll();
  updateWatches();
}

// ── Public interface ─────────────────────────────────────────────────────────

void ConfigService::addReloadCallback(ReloadCallback callback, std::string_view label) {
  m_reloadCallbacks.push_back({std::move(callback), std::string(label)});
}

void ConfigService::setNotificationManager(ConfigNotificationSink* sink) {
  m_notificationManager = sink;
  if (m_notificationManager != nullptr && !m_pendingError.empty()) {
    ConfigProblem pendingError = std::move(m_pendingError);
    m_pendingError = {};
    DeferredCall::callLater([this, pendingError = std::move(pendingError)] mutable {
      if (m_notificationManager == nullptr) {
        m_pendingError = std::move(pendingError);
        return;
      }
      setConfigParseError(std::move(pendingError));
    });
  }
  if (m_notificationManager != nullptr && m_legacyReminderPending) {
    DeferredCall::callLater([this] { notifyLegacyConfigIssues(); });
  }
}

void ConfigService::forceReload() {
  const auto oldDefault = m_defaultWallpaperPath;
  const auto oldLast = m_lastWallpaperPath;
  const auto oldMonitors = m_monitorWallpaperPaths;

  loadAll();

  const bool wallpaperChanged =
      (oldDefault != m_defaultWallpaperPath
       || oldLast != m_lastWallpaperPath
       || oldMonitors != m_monitorWallpaperPaths);
  if (wallpaperChanged && m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
  fireReloadCallbacks();
}

void ConfigService::fireReloadCallbacks() {
  if (!nythera::profiling::enabled()) {
    for (const auto& sub : m_reloadCallbacks) {
      sub.callback();
    }
    return;
  }

  {
    std::string changed;
    const auto add = [&](bool on, const char* name) {
      if (on) {
        changed += changed.empty() ? name : std::string(", ") + name;
      }
    };
    add(m_lastChange.bars, "bars");
    add(m_lastChange.widgets, "widgets");
    add(m_lastChange.desktopWidgets, "desktopWidgets");
    add(m_lastChange.hotCorners, "hotCorners");
    add(m_lastChange.lockscreenWidgets, "lockscreenWidgets");
    add(m_lastChange.wallpaper, "wallpaper");
    add(m_lastChange.backdrop, "backdrop");
    add(m_lastChange.lockscreen, "lockscreen");
    add(m_lastChange.dock, "dock");
    add(m_lastChange.shell, "shell");
    add(m_lastChange.osd, "osd");
    add(m_lastChange.notification, "notification");
    add(m_lastChange.weather, "weather");
    add(m_lastChange.calendar, "calendar");
    add(m_lastChange.system, "system");
    add(m_lastChange.audio, "audio");
    add(m_lastChange.brightness, "brightness");
    add(m_lastChange.battery, "battery");
    add(m_lastChange.bluetooth, "bluetooth");
    add(m_lastChange.keybinds, "keybinds");
    add(m_lastChange.nightlight, "nightlight");
    add(m_lastChange.location, "location");
    add(m_lastChange.idle, "idle");
    add(m_lastChange.hooks, "hooks");
    add(m_lastChange.theme, "theme");
    add(m_lastChange.controlCenter, "controlCenter");
    add(m_lastChange.plugins, "plugins");
    add(m_lastChange.accessibility, "accessibility");
    kLog.info("reload: changed sections = [{}]", changed.empty() ? "none" : changed);
  }

  nythera::profiling::StopWatch total;
  for (std::size_t i = 0; i < m_reloadCallbacks.size(); ++i) {
    const auto& sub = m_reloadCallbacks[i];
    nythera::profiling::StopWatch one;
    sub.callback();
    const double ms = one.elapsedMs();
    if (ms >= 0.5) {
      kLog.info("reload[{}]: {:.1F} ms", sub.label.empty() ? std::format("#{}", i) : sub.label, ms);
    }
  }
  kLog.info("reload: all subscribers {:.1F} ms", total.elapsedMs());
}

bool ConfigService::shouldRunSetupWizard() const {
  if (!m_config.shell.setupWizardEnabled) {
    return false;
  }
  // Single canonical signal: the marker file. If we have no state dir we cannot
  // persist completion, so never show the wizard (it would loop forever).
  if (m_setupMarkerPath.empty() || std::filesystem::exists(m_setupMarkerPath)) {
    return false;
  }
  // Unmigrated live state keeps the completion marker directly under stateDir().
  const std::string legacyDir = FileUtils::stateDir();
  if (!legacyDir.empty() && std::filesystem::exists(legacyDir + "/.setup-complete")) {
    return false;
  }
  return true;
}

std::optional<bool> ConfigService::stateBool(std::string_view owner, std::string_view key) const {
  return m_stateStore.boolValue(owner, key);
}

bool ConfigService::setStateBool(std::string_view owner, std::string_view key, bool value) {
  if (m_settingsAccess == SettingsAccess::InheritanceOnly) {
    return false;
  }
  return m_stateStore.setBool(owner, key, value);
}

bool ConfigService::stateOwnerValid(std::string_view owner) const { return m_stateStore.ownerValid(owner); }

bool ConfigService::stateContains(std::string_view owner, std::string_view key) const {
  return m_stateStore.contains(owner, key);
}

std::optional<std::vector<std::string>>
ConfigService::stateStringArray(std::string_view owner, std::string_view key) const {
  return m_stateStore.stringArrayValue(owner, key);
}

bool ConfigService::setStateStringArray(
    std::string_view owner, std::string_view key, const std::vector<std::string>& value
) {
  if (m_settingsAccess == SettingsAccess::InheritanceOnly) {
    return false;
  }
  return m_stateStore.setStringArray(owner, key, value);
}

std::optional<std::string> ConfigService::stateString(std::string_view owner, std::string_view key) const {
  return m_stateStore.stringValue(owner, key);
}

bool ConfigService::setStateString(std::string_view owner, std::string_view key, std::string_view value) {
  if (m_settingsAccess == SettingsAccess::InheritanceOnly) {
    return false;
  }
  return m_stateStore.setString(owner, key, value);
}

bool ConfigService::clearStateOwner(std::string_view owner) {
  if (m_settingsAccess == SettingsAccess::InheritanceOnly) {
    return false;
  }
  return m_stateStore.clearOwner(owner);
}

Config ConfigService::makeDefaultConfig() {
  Config config;
  nythera::config::seedBuiltinWidgets(config);
  config.idle.behaviors = defaultIdleBehaviors();
  config.bars.push_back(BarConfig{});
  config.controlCenter.shortcuts = defaultControlCenterShortcuts();
  config.shell.session.actions = defaultSessionPanelActions();
  config.plugins.sources = defaultPluginSources();
  return config;
}

void ConfigService::checkReload() {
  if (m_inotify.fd() < 0) {
    return;
  }

  bool configChanged = false;

  m_inotify.drain([this, &configChanged](const inotify_event* event) {
    if ((event->mask & IN_Q_OVERFLOW) != 0) {
      configChanged = true;
      return;
    }
    const auto watch = m_watches.find(event->wd);
    if (watch == m_watches.end()) {
      return;
    }
    if (event->len == 0) {
      configChanged = true;
      return;
    }

    const std::string_view name{event->name};
    if (!watch->second.names.contains(std::string(name)) && !(watch->second.scanToml && name.ends_with(".toml"))) {
      return;
    }

    // An ordinary new regular file is read after its writer closes it. Symlinks and new
    // directories (e.g. ~/.config/nythera/shell) have no close-write event and apply now.
    if ((event->mask & IN_CREATE) != 0) {
      if ((event->mask & IN_ISDIR) != 0) {
        const auto createdDir = canonicalWatchPath(watch->second.directory / name);
        if (std::ranges::any_of(m_watches, [&](const auto& entry) { return entry.second.directory == createdDir; })) {
          return;
        }
      } else {
        std::error_code ec;
        if (!std::filesystem::is_symlink(watch->second.directory / name, ec)) {
          return;
        }
      }
    }

    const auto filePath = watch->second.directory / name;
    if (const auto selfIt = m_selfWrites.find(filePath);
        selfIt != m_selfWrites.end() && (event->mask & IN_MOVED_TO) != 0) {
      std::ifstream input(selfIt->first, std::ios::binary);
      if (input && std::string(std::istreambuf_iterator<char>(input), {}) == selfIt->second) {
        return;
      }
    }

    configChanged = true;
  });
  m_selfWrites.clear();

  if (!configChanged) {
    return;
  }

  // Editors save in several steps (truncate then write, rename the old file away then write the
  // new one, write a temp file then rename it over). Reload once the burst settles so a half-saved
  // or briefly missing file is never applied.
  m_pendingConfigReload = true;
  m_watchReloadTimer.start(kWatchReloadDebounce, [this] { applyPendingWatchReload(); });
}

void ConfigService::applyPendingWatchReload() {
  if (!std::exchange(m_pendingConfigReload, false)) {
    return;
  }

  const auto oldDefault = m_defaultWallpaperPath;
  const auto oldLast = m_lastWallpaperPath;
  const auto oldMonitors = m_monitorWallpaperPaths;

  kLog.info("config changed, reloading");
  loadAll();
  const bool wallpaperChanged =
      (oldDefault != m_defaultWallpaperPath
       || oldLast != m_lastWallpaperPath
       || oldMonitors != m_monitorWallpaperPaths);
  if (wallpaperChanged && m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
  fireReloadCallbacks();
}

BarConfig ConfigService::resolveForOutput(const BarConfig& base, const WaylandOutput& output) {
  BarConfig resolved = base;

  for (const auto& ovr : base.monitorOverrides) {
    if (!outputMatchesSelector(ovr.match, output)) {
      continue;
    }

    kLog.debug("monitor override \"{}\" matched output {} ({})", ovr.match, output.connectorName, output.description);

    if (ovr.position)
      resolved.position = *ovr.position;
    if (ovr.enabled)
      resolved.enabled = *ovr.enabled;
    if (ovr.autoHide)
      resolved.autoHide = *ovr.autoHide;
    if (ovr.autoHideDelayMs)
      resolved.autoHideDelayMs = *ovr.autoHideDelayMs;
    if (ovr.smartAutoHide)
      resolved.smartAutoHide = *ovr.smartAutoHide;
    if (ovr.showOnWorkspaceSwitch)
      resolved.showOnWorkspaceSwitch = *ovr.showOnWorkspaceSwitch;
    if (ovr.reserveSpace)
      resolved.reserveSpace = *ovr.reserveSpace;
    if (ovr.layer)
      resolved.layer = *ovr.layer;
    if (ovr.thickness)
      resolved.thickness = *ovr.thickness;
    if (ovr.backgroundOpacity)
      resolved.backgroundOpacity = *ovr.backgroundOpacity;
    if (ovr.transparencyMode)
      resolved.transparencyMode = *ovr.transparencyMode;
    if (ovr.surfaceMode)
      resolved.surfaceMode = *ovr.surfaceMode;
    if (ovr.dynamicCenter)
      resolved.dynamicCenter = *ovr.dynamicCenter;
    if (ovr.dynamicNotifications)
      resolved.dynamicNotifications = *ovr.dynamicNotifications;
    if (ovr.dynamicTimers)
      resolved.dynamicTimers = *ovr.dynamicTimers;
    if (ovr.dynamicAlarms)
      resolved.dynamicAlarms = *ovr.dynamicAlarms;
    if (ovr.dynamicCalls)
      resolved.dynamicCalls = *ovr.dynamicCalls;
    if (ovr.dynamicTimerSeconds)
      resolved.dynamicTimerSeconds = *ovr.dynamicTimerSeconds;
    if (ovr.dynamicAlarmTime)
      resolved.dynamicAlarmTime = *ovr.dynamicAlarmTime;
    if (ovr.dynamicSnoozeSeconds)
      resolved.dynamicSnoozeSeconds = *ovr.dynamicSnoozeSeconds;
    if (ovr.dynamicActivityIndicators)
      resolved.dynamicActivityIndicators = *ovr.dynamicActivityIndicators;
    if (ovr.dynamicPrivacy)
      resolved.dynamicPrivacy = *ovr.dynamicPrivacy;
    if (ovr.dynamicDevices)
      resolved.dynamicDevices = *ovr.dynamicDevices;
    if (ovr.dynamicVisualizer)
      resolved.dynamicVisualizer = *ovr.dynamicVisualizer;
    if (ovr.dynamicMedia)
      resolved.dynamicMedia = *ovr.dynamicMedia;
    if (ovr.dynamicVolume)
      resolved.dynamicVolume = *ovr.dynamicVolume;
    if (ovr.dynamicBrightness)
      resolved.dynamicBrightness = *ovr.dynamicBrightness;
    if (ovr.dynamicHoverExpand)
      resolved.dynamicHoverExpand = *ovr.dynamicHoverExpand;
    if (ovr.dynamicDwellMs)
      resolved.dynamicDwellMs = *ovr.dynamicDwellMs;
    if (ovr.dynamicWidth)
      resolved.dynamicWidth = *ovr.dynamicWidth;
    if (ovr.dynamicClockFormat)
      resolved.dynamicClockFormat = *ovr.dynamicClockFormat;
    if (ovr.compositorBlur)
      resolved.compositorBlur = *ovr.compositorBlur;
    if (ovr.border)
      resolved.border = *ovr.border;
    if (ovr.borderWidth)
      resolved.borderWidth = *ovr.borderWidth;
    if (ovr.radius) {
      resolved.radius = *ovr.radius;
      resolved.radiusTopLeft = *ovr.radius;
      resolved.radiusTopRight = *ovr.radius;
      resolved.radiusBottomLeft = *ovr.radius;
      resolved.radiusBottomRight = *ovr.radius;
    }
    if (ovr.radiusTopLeft)
      resolved.radiusTopLeft = *ovr.radiusTopLeft;
    if (ovr.radiusTopRight)
      resolved.radiusTopRight = *ovr.radiusTopRight;
    if (ovr.radiusBottomLeft)
      resolved.radiusBottomLeft = *ovr.radiusBottomLeft;
    if (ovr.radiusBottomRight)
      resolved.radiusBottomRight = *ovr.radiusBottomRight;
    if (ovr.concaveEdgeCorners)
      resolved.concaveEdgeCorners = *ovr.concaveEdgeCorners;
    if (ovr.marginEnds)
      resolved.marginEnds = *ovr.marginEnds;
    if (ovr.marginEdge)
      resolved.marginEdge = *ovr.marginEdge;
    if (ovr.marginOppositeEdge)
      resolved.marginOppositeEdge = *ovr.marginOppositeEdge;
    if (ovr.padding)
      resolved.padding = *ovr.padding;
    if (ovr.widgetSpacing)
      resolved.widgetSpacing = *ovr.widgetSpacing;
    if (ovr.shadow)
      resolved.shadow = *ovr.shadow;
    if (ovr.contactShadow)
      resolved.contactShadow = *ovr.contactShadow;
    if (ovr.panelOverlap)
      resolved.panelOverlap = *ovr.panelOverlap;
    if (ovr.capsuleThickness)
      resolved.capsuleThickness = *ovr.capsuleThickness;
    if (ovr.fontFamily)
      resolved.fontFamily = *ovr.fontFamily;
    if (ovr.startWidgets)
      resolved.startWidgets = *ovr.startWidgets;
    if (ovr.centerWidgets)
      resolved.centerWidgets = *ovr.centerWidgets;
    if (ovr.endWidgets)
      resolved.endWidgets = *ovr.endWidgets;
    if (ovr.scale)
      resolved.scale = *ovr.scale;
    if (ovr.fontScale)
      resolved.fontScale = *ovr.fontScale;
    if (ovr.widgetCapsuleDefault)
      resolved.widgetCapsuleDefault = *ovr.widgetCapsuleDefault;
    if (ovr.widgetCapsuleFill)
      resolved.widgetCapsuleFill = *ovr.widgetCapsuleFill;
    if (ovr.widgetCapsuleBorderSpecified) {
      resolved.widgetCapsuleBorderSpecified = true;
      resolved.widgetCapsuleBorder = ovr.widgetCapsuleBorder;
    }
    if (ovr.widgetCapsuleForeground) {
      resolved.widgetCapsuleForeground = *ovr.widgetCapsuleForeground;
    }
    if (ovr.widgetColor) {
      resolved.widgetColor = *ovr.widgetColor;
    }
    if (ovr.widgetIconColor) {
      resolved.widgetIconColor = *ovr.widgetIconColor;
    }
    if (ovr.widgetCapsuleGroups) {
      resolved.widgetCapsuleGroups = *ovr.widgetCapsuleGroups;
    }
    if (ovr.widgetCapsulePadding) {
      resolved.widgetCapsulePadding = std::clamp(static_cast<float>(*ovr.widgetCapsulePadding), 0.0F, 48.0F);
    }
    if (ovr.widgetCapsuleRadius.has_value()) {
      resolved.widgetCapsuleRadius = std::clamp(*ovr.widgetCapsuleRadius, 0.0, 80.0);
    }
    if (ovr.widgetCapsuleOpacity) {
      resolved.widgetCapsuleOpacity = std::clamp(static_cast<float>(*ovr.widgetCapsuleOpacity), 0.0F, 1.0F);
    }
    if (ovr.widgetCapsuleBorderWidth) {
      resolved.widgetCapsuleBorderWidth = nythera::config::schema::applyRange(
          *ovr.widgetCapsuleBorderWidth, nythera::config::schema::kBarCapsuleBorderWidthRange
      );
    }
    if (ovr.hoverHighlight) {
      resolved.hoverHighlight = *ovr.hoverHighlight;
    }
    if (ovr.showTooltip) {
      resolved.showTooltip = *ovr.showTooltip;
    }
    if (ovr.deadZone.actions) {
      resolved.deadZone.actions = *ovr.deadZone.actions;
    }
    if (ovr.deadZone.clickThrough) {
      resolved.deadZone.clickThrough = *ovr.deadZone.clickThrough;
    }
    break; // first match wins
  }

  resolved.backgroundOpacity =
      barBackgroundOpacityForTransparencyMode(resolved.transparencyMode, resolved.backgroundOpacity);
  return resolved;
}

DockConfig ConfigService::resolveForOutput(const DockConfig& base, const WaylandOutput& output) {
  for (const auto& ovr : base.monitorOverrides) {
    if (!outputMatchesSelector(ovr.match, output)) {
      continue;
    }

    kLog.debug(
        "dock monitor override \"{}\" matched output {} ({})", ovr.match, output.connectorName, output.description
    );

    return resolveDockMonitorOverride(base, ovr);
  }

  DockConfig resolved = base;
  resolved.monitorOverrides.clear();
  return resolved;
}

// ── Private helpers ──────────────────────────────────────────────────────────

void ConfigService::updateWatches() {
  if (m_inotify.fd() < 0 || (m_configDir.empty() && m_sharedThemePath.empty() && m_stateStore.path().empty())) {
    return;
  }

  std::map<std::filesystem::path, WatchEntry> desired;
  const auto add = [&](std::filesystem::path path, bool contents) {
    if (path.empty()) {
      return;
    }
    std::error_code error;
    std::filesystem::path name;
    if (!contents) {
      name = path.filename();
      path = path.parent_path();
    }
    while (!path.empty() && !std::filesystem::is_directory(path, error)) {
      name = path.filename();
      const auto parent = path.parent_path();
      if (parent == path) {
        return;
      }
      path = parent;
    }
    if (path.empty()) {
      return;
    }
    const auto resolved = canonicalWatchPath(path);
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
  if (m_usingLegacyConfigDir) {
    if (const std::string shellDir = FileUtils::applicationConfigDir(FileUtils::Application::Shell);
        !shellDir.empty()) {
      add(shellDir, true);
      add(shellDir, false);
    }
    if (!m_legacySettingsPath.empty()) {
      const std::filesystem::path legacyPath(m_legacySettingsPath);
      add(legacyPath, false);
      const auto target = canonicalWatchPath(legacyPath);
      if (target != legacyPath) {
        add(target, false);
      }
      std::error_code ec;
      const auto link = std::filesystem::read_symlink(legacyPath, ec);
      if (!ec) {
        add(link.is_absolute() ? link : legacyPath.parent_path() / link, false);
      }
    }
  }

  for (const auto& dir : m_includeDirs) {
    add(dir, true);
  }
  const auto watchFilePath = [&](const std::filesystem::path& path) {
    add(path, false);
    const auto target = canonicalWatchPath(path);
    if (target != path) {
      add(target, false);
    }
    std::error_code ec;
    const auto link = std::filesystem::read_symlink(path, ec);
    if (!ec) {
      add(link.is_absolute() ? link : path.parent_path() / link, false);
    }
  };
  for (const auto& path : m_watchPaths) {
    watchFilePath(path);
  }
  for (const auto& path : m_includeLoadedFiles) {
    watchFilePath(path);
  }
  if (!m_sharedThemePath.empty()) {
    watchFilePath(m_sharedThemePath);
  }
  if (m_settingsAccess == SettingsAccess::InheritanceOnly && !m_stateStore.path().empty()) {
    watchFilePath(m_stateStore.path());
  }

  std::map<int, WatchEntry> watches;
  for (auto& [path, watch] : desired) {
    if (const auto descriptor = m_inotify.watch(path.c_str(), inotifyMask)) {
      watches.emplace(*descriptor, std::move(watch));
    }
  }
  for (const auto& [descriptor, watch] : m_watches) {
    (void)watch;
    if (!watches.contains(descriptor)) {
      m_inotify.unwatch(descriptor);
    }
  }
  m_watches = std::move(watches);
}

bool ConfigService::overlayLegacySidecar(
    toml::table& merged, nythera::config::ConfigOriginIndex& origins, toml::table& sidecar, bool& hasSidecar,
    ConfigProblem* errorOut
) const {
  sidecar.clear();
  hasSidecar = false;
  if (!m_usingLegacyConfigDir || m_legacySettingsPath.empty()) {
    return true;
  }

  std::error_code ec;
  const std::filesystem::path settingsFile(m_legacySettingsPath);
  if (!std::filesystem::exists(settingsFile, ec) || ec) {
    return true;
  }

  try {
    sidecar = toml::parse_file(m_legacySettingsPath);
  } catch (const toml::parse_error& e) {
    auto origin = nythera::config::parseErrorOrigin(e, settingsFile);
    kLog.warn("{}", origin.prefixed(e.description()));
    if (errorOut != nullptr && errorOut->empty()) {
      *errorOut = ConfigProblem{std::move(origin), std::string(e.description())};
    }
    sidecar.clear();
    return false;
  }

  origins.record(settingsFile, sidecar);
  seedLegacyDockAllowListEnabled(sidecar, merged);

  schema::Diagnostics migrationDiag;
  if (const auto parsedVersion = nythera::config::storedConfigVersion(sidecar, migrationDiag);
      parsedVersion.has_value()) {
    const int appliedVersion =
        nythera::config::applyPendingConfigMigrations(sidecar, merged, *parsedVersion, migrationDiag);
    sidecar.insert_or_assign(nythera::config::kConfigVersionKey, static_cast<std::int64_t>(appliedVersion));
  }
  origins.annotate(migrationDiag);

  bool hasMigrationError = false;
  for (const auto& entry : migrationDiag.entries) {
    if (entry.severity == schema::Diagnostics::Severity::Error) {
      hasMigrationError = true;
      if (errorOut != nullptr && errorOut->empty()) {
        *errorOut = ConfigProblem::from(entry);
      }
    } else {
      kLog.warn("{}", entry.describe());
    }
  }
  if (hasMigrationError) {
    return false;
  }

  deepMerge(merged, sidecar);
  hasSidecar = true;
  return true;
}

void ConfigService::setConfigParseError(ConfigProblem problem) {
  if (problem.empty()) {
    // Dismiss any previous config-error notification.
    if (m_notificationManager != nullptr && m_configErrorNotificationId != 0) {
      m_notificationManager->dismissConfigNotice(m_configErrorNotificationId);
      m_configErrorNotificationId = 0;
    }
    m_pendingError = {};
    return;
  }

  if (m_notificationManager == nullptr) {
    m_pendingError = std::move(problem);
    return;
  }

  if (m_configErrorNotificationId != 0) {
    m_notificationManager->dismissConfigNotice(m_configErrorNotificationId);
  }
  // Title carries the location so the body has room for the whole message; with
  // no location to show, the title says what kind of problem this is instead.
  const bool located = problem.origin.valid();
  std::string title = located ? problem.origin.shortFormat(m_configDir) : std::string("Config error");
  std::string body = located ? "Error: " + problem.message : std::move(problem.message);
  m_configErrorNotificationId = m_notificationManager->postConfigError(std::move(title), std::move(body));
}

void ConfigService::updateLegacyConfigIssues(nythera::config::LegacyConfigIssues issues) {
  std::ranges::sort(issues, [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.migrationVersion, lhs.path) < std::tie(rhs.migrationVersion, rhs.path);
  });
  issues.erase(
      std::ranges::unique(
          issues, {}, [](const auto& issue) { return std::tie(issue.migrationVersion, issue.path); }
      ).begin(),
      issues.end()
  );

  const std::string fingerprint = nythera::config::legacyConfigIssueFingerprint(issues);
  if (fingerprint != m_loggedLegacyIssueFingerprint) {
    for (const auto& issue : issues) {
      kLog.warn(
          "{}", issue.origin.prefixed(issue.path + ": " + issue.message + " (source configuration needs updating)")
      );
    }
    m_loggedLegacyIssueFingerprint = fingerprint;
  }
  m_legacyConfigIssues = std::move(issues);

  if (m_settingsAccess != SettingsAccess::ReadWrite) {
    return;
  }

  if (m_legacyConfigIssues.empty()) {
    m_legacyReminderPending = false;
    m_legacyReminderTimer.stop();
    if (m_stateStore.stringValue(kMigrationReminderOwner, kMigrationReminderKey).has_value()) {
      (void)m_stateStore.clearOwner(kMigrationReminderOwner);
    }
    return;
  }

  const std::int64_t now = currentEpochSeconds();
  const auto encodedState = m_stateStore.stringValue(kMigrationReminderOwner, kMigrationReminderKey);
  const auto reminderState = encodedState.has_value() ? parseMigrationReminderState(*encodedState) : std::nullopt;
  const bool hasNewIssues = !reminderState.has_value()
      || nythera::config::legacyConfigFingerprintHasNewIssues(fingerprint, reminderState->issueFingerprint);
  const bool intervalElapsed = !reminderState.has_value()
      || nythera::config::legacyConfigReminderIntervalElapsed(now, reminderState->epochSeconds);
  m_legacyReminderPending = hasNewIssues || intervalElapsed;
  if (m_legacyReminderPending) {
    notifyLegacyConfigIssues();
    return;
  }

  const auto elapsed = std::chrono::seconds(now - reminderState->epochSeconds);
  const auto remaining = std::chrono::seconds(nythera::config::kLegacyConfigReminderIntervalSeconds) - elapsed;
  m_legacyReminderTimer.start(std::chrono::duration_cast<std::chrono::milliseconds>(remaining), [this] {
    m_legacyReminderPending = true;
    notifyLegacyConfigIssues();
  });
}

void ConfigService::notifyLegacyConfigIssues() {
  if (!m_legacyReminderPending || m_notificationManager == nullptr || m_legacyConfigIssues.empty()) {
    return;
  }

  const auto& issue = m_legacyConfigIssues.front();
  const std::string fingerprint = nythera::config::legacyConfigIssueFingerprint(m_legacyConfigIssues);
  const std::int64_t now = currentEpochSeconds();
  std::string title = issue.origin.valid() ? issue.origin.shortFormat(m_configDir)
                                           : i18n::tr("notifications.internal.config-migration-title");
  (void)m_notificationManager->postConfigMigration(
      std::move(title),
      i18n::tr("notifications.internal.config-migration-body", "path", issue.path + ": " + issue.message)
  );
  (void)m_stateStore.setString(kMigrationReminderOwner, kMigrationReminderKey, std::format("{}\n{}", now, fingerprint));
  m_legacyReminderPending = false;
  m_legacyReminderTimer.start(
      std::chrono::milliseconds(nythera::config::kLegacyConfigReminderIntervalSeconds * 1000), [this] {
        m_legacyReminderPending = true;
        notifyLegacyConfigIssues();
      }
  );
}

namespace {

  void mergeSessionActions(toml::table& base, const toml::array& overlay) {
    const auto* baseActions = base["actions"].as_array();
    if (baseActions == nullptr) {
      base.insert_or_assign("actions", overlay);
      return;
    }

    toml::array merged;
    for (std::size_t index = 0; index < overlay.size(); ++index) {
      const toml::node& overlayNode = overlay[index];
      const toml::table* overlayAction = overlayNode.as_table();
      const toml::table* baseAction = index < baseActions->size() ? (*baseActions)[index].as_table() : nullptr;
      if (overlayAction == nullptr || baseAction == nullptr) {
        merged.push_back(overlayNode);
        continue;
      }

      const auto baseName = (*baseAction)["action"].value<std::string>();
      const auto overlayName = (*overlayAction)["action"].value<std::string>();
      if (baseName != overlayName) {
        merged.push_back(overlayNode);
        continue;
      }

      toml::table mergedAction = *baseAction;
      ConfigService::deepMerge(mergedAction, *overlayAction);
      merged.push_back(std::move(mergedAction));
    }
    base.insert_or_assign("actions", std::move(merged));
  }

} // namespace

void ConfigService::deepMerge(toml::table& base, const toml::table& overlay) {
  for (const auto& [k, v] : overlay) {
    if (const auto* overlayTbl = v.as_table()) {
      if (auto* baseNode = base.get(k)) {
        if (auto* baseTbl = baseNode->as_table()) {
          if (k == "session") {
            if (const auto* overlayActions = (*overlayTbl)["actions"].as_array()) {
              mergeSessionActions(*baseTbl, *overlayActions);
            }
            toml::table sessionOverlay = *overlayTbl;
            sessionOverlay.erase("actions");
            deepMerge(*baseTbl, sessionOverlay);
            continue;
          }
          deepMerge(*baseTbl, *overlayTbl);
          continue;
        }
      }
    }
    // Tables-over-non-tables, non-tables, and arrays: overlay replaces base wholesale.
    base.insert_or_assign(k, v);
  }
}

void ConfigService::loadAll() {
  nythera::profiling::ScopedTimer parseTimer(kLog, "reload: parse (loadAll)");
  m_effectiveOverrideCache.clear();

  const ActiveConfigLocation location = resolveActiveConfigLocation();
  m_configDir = location.configDir;
  m_sharedThemePath = location.sharedThemePath;
  m_legacySettingsPath = location.legacySettingsPath;
  m_usingLegacyConfigDir = location.usingLegacy;

  if (m_settingsAccess == SettingsAccess::InheritanceOnly) {
    if (const std::string stateDir = resolveActiveStateDir(); !stateDir.empty()) {
      m_stateStore.setPath(stateDir + "/state.toml");
      m_stateStore.loadReadOnly();
    }
  }

  Config nextConfig;
  nythera::config::seedBuiltinWidgets(nextConfig);

  std::vector<std::filesystem::path> baseFiles;
  if (!m_sharedThemePath.empty()) {
    baseFiles.emplace_back(m_sharedThemePath);
  }
  auto loaded = nythera::config::mergeConfigWithSources(m_configDir, {}, baseFiles);
  toml::table merged = std::move(loaded.config.merged);
  std::string firstError = std::move(loaded.config.firstError);
  const nythera::config::schema::SourceOrigin firstErrorOrigin = std::move(loaded.config.firstErrorOrigin);
  m_includeLoadedFiles = std::move(loaded.config.loadedFiles);
  m_includeDirs = std::move(loaded.config.includeDirs);
  m_watchPaths = std::move(loaded.watchPaths);

  nythera::config::ConfigOriginIndex origins = std::move(loaded.config.origins);
  toml::table legacySidecar;
  bool hasLegacySidecar = false;
  ConfigProblem legacySidecarError;
  (void)overlayLegacySidecar(merged, origins, legacySidecar, hasLegacySidecar, &legacySidecarError);

  toml::table userTable = merged;
  userTable.erase(nythera::config::kConfigVersionKey);

  merged.erase(nythera::config::kConfigVersionKey);
  nythera::config::LegacyConfigIssues legacyIssues;
  nythera::config::normalizeLegacyConfig(merged, legacyIssues, hasLegacySidecar ? &legacySidecar : nullptr);
  for (auto& issue : legacyIssues) {
    if (const auto* origin = origins.find(issue.path)) {
      issue.origin = *origin;
    }
  }

  ConfigProblem semanticError = !firstError.empty() ? ConfigProblem{firstErrorOrigin, std::move(firstError)}
      : !legacySidecarError.empty()                 ? std::move(legacySidecarError)
                                                    : ConfigProblem{};

  if (semanticError.empty() && m_includeLoadedFiles.empty() && !hasLegacySidecar) {
    kLog.info("no config files found, using defaults");
    m_lastChange = ConfigChangeSet{};
    m_config = makeDefaultConfig();
    m_sourceMap = std::move(loaded.sources);
    m_mergedUserTable = toml::table{};
    extractWallpaperFromTable(toml::table{});
    updateLegacyConfigIssues({});
    setConfigParseError({});
    m_configTable = std::make_shared<const toml::table>(config_export::serialize(m_config));
    updateWatches();
    return;
  }

  ConfigProblem diagnosticError;
  schema::Diagnostics diagnostics;
  if (semanticError.empty()) {
    try {
      if (m_validator) {
        diagnostics = m_validator(merged, origins);
      }
      std::size_t errorCount = 0;
      for (const auto& entry : diagnostics.entries) {
        if (entry.severity == schema::Diagnostics::Severity::Error) {
          if (entry.recoveryScope == schema::Diagnostics::RecoveryScope::Document) {
            if (semanticError.empty()) {
              semanticError = ConfigProblem::from(entry);
            }
            kLog.warn("{}", entry.describe());
            continue;
          }
          ++errorCount;
          if (diagnosticError.empty()) {
            diagnosticError = ConfigProblem::from(entry);
          }
        }
        kLog.warn("{}", entry.describe());
      }
      if (errorCount > 1) {
        diagnosticError.message += std::format(" (and {} more config errors)", errorCount - 1);
      }
    } catch (const std::exception& e) {
      semanticError = ConfigProblem{{}, e.what()};
      kLog.warn("config validation error: {}", e.what());
    }
  }
  if (semanticError.empty()) {
    try {
      parseConfigTable(merged, nextConfig, true, false);
      restoreInvalidComponents(nextConfig, m_config, diagnostics);
    } catch (const std::exception& e) {
      semanticError = ConfigProblem{{}, e.what()};
      kLog.warn("config parse error: {}", e.what());
    }
  }

  if (semanticError.empty()) {
    m_lastChange = computeConfigChangeSet(m_config, nextConfig);
    m_config = std::move(nextConfig);
    m_sourceMap = std::move(loaded.sources);
    m_mergedUserTable = std::move(userTable);
    extractWallpaperFromTable(merged);
    updateLegacyConfigIssues(std::move(legacyIssues));
  } else if (m_config.bars.empty()) {
    m_lastChange = ConfigChangeSet{};
    m_config = makeDefaultConfig();
    m_sourceMap = nythera::config::ConfigSourceMap{};
    m_mergedUserTable = toml::table{};
    extractWallpaperFromTable(toml::table{});
  } else {
    // Parse error with a usable previous config retained — fan out conservatively.
    m_lastChange = ConfigChangeSet{};
  }

  // semanticError already absorbed the merge / legacy sidecar / migration errors above.
  setConfigParseError(!semanticError.empty() ? std::move(semanticError) : std::move(diagnosticError));

  // Included files may live in subdirectories or absolute paths outside the config
  // dir, and the include set can change on every reload — reconcile their watches.
  updateWatches();

  m_configTable = std::make_shared<const toml::table>(config_export::serialize(m_config));
}

void ConfigService::parseConfigTable(
    const toml::table& tbl, Config& config, bool logSummary, bool logSchemaDiagnostics
) {
  // Diagnostics raised by schema-driven sections (e.g. unknown enum values).
  // Flushed to the log below, preserving the legacy warn-and-continue behavior.
  schema::Diagnostics schemaDiag;

  // Parse [bar.*] named subtables
  if (const auto* barTblMap = tbl["bar"].as_table()) {
    std::vector<BarConfig> parsedBars;
    for (const auto& [barName, barNode] : *barTblMap) {
      const auto* barTbl = barNode.as_table();
      if (barTbl == nullptr) {
        continue;
      }

      BarConfig bar;
      bar.name = std::string(barName.str());
      // position is read explicitly (the base bar always emits it; monitor
      // overrides emit it conditionally), the rest via the shared schema.
      if (auto v = (*barTbl)["position"].value<std::string>()) {
        bar.position = *v;
      }
      readConfigSection(*barTbl, bar, schema::barFieldsSchema(), "bar." + bar.name, schemaDiag);

      // Parse [bar.<name>.monitor.*] overrides — insertion order preserved by toml++.
      if (const auto* monTblMap = (*barTbl)["monitor"].as_table()) {
        for (const auto& [monName, monNode] : *monTblMap) {
          const auto* monTbl = monNode.as_table();
          if (monTbl == nullptr) {
            continue;
          }
          BarMonitorOverride ovr;
          ovr.match = std::string(monName.str()); // key is the match unless an explicit `match` overrides it
          readConfigSection(
              *monTbl, ovr, schema::barMonitorOverrideSchema(),
              "bar." + bar.name + ".monitor." + std::string(monName.str()), schemaDiag
          );
          bar.monitorOverrides.push_back(std::move(ovr));
        }
      }

      parsedBars.push_back(std::move(bar));
    }

    std::vector<std::string> order;
    if (const auto* orderNode = (*barTblMap)["order"].as_array()) {
      order = readStringArray(*orderNode);
    }

    std::vector<bool> used(parsedBars.size(), false);
    for (const auto& orderedName : order) {
      for (std::size_t i = 0; i < parsedBars.size(); ++i) {
        if (!used[i] && parsedBars[i].name == orderedName) {
          used[i] = true;
          config.bars.push_back(std::move(parsedBars[i]));
          break;
        }
      }
    }

    for (std::size_t i = 0; i < parsedBars.size(); ++i) {
      if (!used[i]) {
        config.bars.push_back(std::move(parsedBars[i]));
      }
    }
  }

  // Parse [widget.*] — named widget instances with per-widget settings
  if (const auto* widgetTbl = tbl["widget"].as_table()) {
    for (const auto& [name, node] : *widgetTbl) {
      const auto* entryTbl = node.as_table();
      if (entryTbl == nullptr) {
        continue;
      }

      const std::string widgetName(name.str());
      WidgetConfig wc = nythera::config::readBarWidgetConfig(widgetName, *entryTbl, config);

      try {
        config.widgets[widgetName] = std::move(wc);
      } catch (const std::exception& e) {
        config.widgets.erase(widgetName);
        kLog.warn("widget.{}: {}", widgetName, e.what());
      }
    }
  }

  // Every schema-backed section is read through the section registry, so the loader
  // cannot recognize a section that the validator and exporter do not.
  for (const schema::SectionSpec& spec : schema::sections()) {
    const auto* sectionTbl = tbl[spec.name].as_table();
    if (sectionTbl == nullptr) {
      continue;
    }
    try {
      spec.read(*sectionTbl, config, schemaDiag);
    } catch (const std::exception& e) {
      schemaDiag.error(std::string(spec.name), e.what());
      kLog.warn("{}: {}", spec.name, e.what());
    }
  }

  // Template config files (e.g. user-templates.toml) store palette extensions under
  // [config.custom_colors]; lift them into the canonical theme.templates slot.
  schema::liftTemplateConfigCustomColors(tbl, config, schemaDiag);

  // Default seeding must apply even when the section is absent, so it runs after the
  // registry pass. A schema read can't tell an explicitly empty list from a missing
  // one, so these probe the raw table for the list key.
  const auto hasExplicitArray = [&tbl](std::string_view section, std::string_view key) {
    const auto* sectionTbl = tbl[section].as_table();
    return sectionTbl != nullptr && (*sectionTbl)[key].as_array() != nullptr;
  };
  const bool sessionActionsConfigured = [&tbl] {
    const auto* shellTbl = tbl["shell"].as_table();
    const auto* sessionTbl = shellTbl != nullptr ? (*shellTbl)["session"].as_table() : nullptr;
    return sessionTbl != nullptr && (*sessionTbl)["actions"].as_array() != nullptr;
  }();
  if (!sessionActionsConfigured && config.shell.session.actions.empty()) {
    config.shell.session.actions = defaultSessionPanelActions();
  }
  if (!hasExplicitArray("control_center", "shortcuts") && config.controlCenter.shortcuts.empty()) {
    config.controlCenter.shortcuts = defaultControlCenterShortcuts();
  }
  if (!hasExplicitArray("plugins", "source") && config.plugins.sources.empty()) {
    config.plugins.sources = defaultPluginSources();
  }
  if (config.idle.behaviors.empty()) {
    config.idle.behaviors = defaultIdleBehaviors();
  }

  // Launcher providers are resolved after the registry pass, once config.shell is
  // populated. An empty common prefix falls back to '/', and providers that name
  // nothing real (or a disabled plugin, or the fixed Applications provider) are
  // dropped loudly rather than silently ignored.
  if (config.shell.launcher.providerPrefix.empty()) {
    schemaDiag.warn("shell.launcher.provider_prefix", "is empty, falling back to '/'");
    config.shell.launcher.providerPrefix = "/";
  }
  std::unordered_set<std::string> enabledPlugins;
  if (const auto* pluginsTbl = tbl["plugins"].as_table()) {
    if (const auto* enabledArr = (*pluginsTbl)["enabled"].as_array()) {
      for (const auto& node : *enabledArr) {
        if (const auto* strVal = node.as_string()) {
          enabledPlugins.insert(StringUtils::toLower(strVal->get()));
        }
      }
    }
  }
  std::erase_if(config.shell.launcher.providers, [&](const LauncherProviderConfig& provider) {
    if (provider.name == "applications") {
      schemaDiag.warn(
          "shell.launcher.providers.applications", "custom settings are not allowed (Applications is always global)"
      );
      return true;
    }
    const auto isBuiltin = std::ranges::contains(launcher::kBuiltinProviders, provider.name);
    if (!isBuiltin) {
      const std::size_t colon = provider.name.find(':');
      bool isPlugin = false;
      std::string pluginIdStr;
      if (colon != std::string::npos) {
        std::string_view pluginId = std::string_view(provider.name).substr(0, colon);
        std::string_view entryName = std::string_view(provider.name).substr(colon + 1);
        if (scripting::isValidPluginId(pluginId) && scripting::isValidPluginIdSegment(entryName)) {
          isPlugin = true;
          pluginIdStr = std::string(pluginId);
        }
      }
      if (!isPlugin) {
        schemaDiag.warn("shell.launcher.providers." + provider.name, "provider is nonexistent");
        return true;
      }
      if (!enabledPlugins.contains(pluginIdStr)) {
        schemaDiag.warn("shell.launcher.providers." + provider.name, "plugin '" + pluginIdStr + "' is not enabled");
        return true;
      }
    }
    return false;
  });

  // Parse [desktop_widgets]
  if (const auto* desktopWidgetsTbl = tbl["desktop_widgets"].as_table()) {
    auto& desktopWidgets = config.desktopWidgets;
    if (auto v = (*desktopWidgetsTbl)["enabled"].value<bool>()) {
      desktopWidgets.enabled = *v;
    }
    if (auto schemaVersion = (*desktopWidgetsTbl)["schema_version"].value<int64_t>()) {
      desktopWidgets.schemaVersion = static_cast<std::int32_t>(*schemaVersion);
    }
    parseWidgetsPlacementSection(*desktopWidgetsTbl, desktopWidgets.grid, desktopWidgets.widgets, "desktop_widgets");
  }

  // Parse [lockscreen_widgets]
  if (const auto* lockscreenWidgetsTbl = tbl["lockscreen_widgets"].as_table()) {
    auto& lockscreenWidgets = config.lockscreenWidgets;
    if (auto v = (*lockscreenWidgetsTbl)["enabled"].value<bool>()) {
      lockscreenWidgets.enabled = *v;
    }
    if (auto schemaVersion = (*lockscreenWidgetsTbl)["schema_version"].value<int64_t>()) {
      lockscreenWidgets.schemaVersion = static_cast<std::int32_t>(*schemaVersion);
    }
    parseWidgetsPlacementSection(
        *lockscreenWidgetsTbl, lockscreenWidgets.grid, lockscreenWidgets.widgets, "lockscreen_widgets"
    );
  }

  // Parse [plugin_settings."author/plugin"] — open-ended per-plugin setting maps,
  // validated against the manifest schema (not the static pluginsSchema). Keys may
  // contain '/', so this is a top-level table rather than nested under [plugins].
  if (const auto* pluginSettingsTbl = tbl["plugin_settings"].as_table()) {
    for (const auto& [pluginId, pluginNode] : *pluginSettingsTbl) {
      const auto* perPlugin = pluginNode.as_table();
      if (perPlugin == nullptr) {
        continue;
      }
      auto& bucket = config.plugins.pluginSettings[std::string(pluginId.str())];
      for (const auto& [key, value] : *perPlugin) {
        if (auto parsed = nythera::config::readWidgetSettingValue(value); parsed.has_value()) {
          bucket[std::string(key.str())] = std::move(*parsed);
        }
      }
    }
  }

  if (config.bars.empty()) {
    if (logSummary) {
      kLog.info("no [bar.*] defined, using defaults");
    }
    config.bars.push_back(BarConfig{});
  }

  if (logSummary) {
    std::string barOrder;
    for (const auto& bar : config.bars) {
      if (!barOrder.empty()) {
        barOrder += ", ";
      }
      barOrder += bar.name;
    }
    kLog.info("{} bar(s) defined", config.bars.size());
    kLog.info("bar order: {}", barOrder);
    kLog.info("idle behaviors={}", config.idle.behaviors.size());
    std::size_t hookKindsUsed = 0;
    for (const auto& cmds : config.hooks.commands) {
      if (!cmds.empty()) {
        ++hookKindsUsed;
      }
    }
    kLog.info("hooks kinds with commands={}", hookKindsUsed);
  }

  if (logSchemaDiagnostics) {
    for (const auto& entry : schemaDiag.entries) {
      kLog.warn("{}: {}", entry.path, entry.message);
    }
  }
}

bool ConfigService::matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const {
  const auto& configured = keybindSet(m_config.keybinds, action);
  const auto active = configured.empty() ? defaultKeybindSet(action) : configured;
  return std::ranges::any_of(active, [sym, modifiers](const KeyChord& chord) {
    return keyChordMatches(chord, sym, modifiers);
  });
}
