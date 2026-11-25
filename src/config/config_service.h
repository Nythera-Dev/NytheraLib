#pragma once

#include "config/config_credentials.h"
#include "config/config_migrations.h"
#include "config/config_origins.h"
#include "config/config_sources.h"
#include "config/config_types.h"
#include "config/schema/diagnostics.h"
#include "config/state_store.h"
#include "config/toml_persistence.h"
#include "core/inotify/inotify.h"
#include "core/timer_manager.h"
#include "core/toml.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class IpcService;
class ConfigNotificationSink;

class ConfigService {
public:
  using ReloadCallback = std::function<void()>;
  using ChangeCallback = std::function<void()>;

  // RAII scope that coalesces wallpaper changes: any setWallpaperPath() calls
  // inside the scope skip the per-call callback, and a single callback is
  // fired on scope exit if anything actually changed.
  class WallpaperBatch {
  public:
    explicit WallpaperBatch(ConfigService& config);
    ~WallpaperBatch();
    WallpaperBatch(const WallpaperBatch&) = delete;
    WallpaperBatch& operator=(const WallpaperBatch&) = delete;

  private:
    ConfigService& m_config;
  };

  using MergedConfigValidator = std::function<
      nythera::config::schema::Diagnostics(const toml::table&, const nythera::config::ConfigOriginIndex&)>;
  using OverrideEffectivenessChecker =
      std::function<bool(const std::vector<std::string>&, const Config&, const Config&)>;
  explicit ConfigService(MergedConfigValidator validator = {}, OverrideEffectivenessChecker effectiveness = {});
  struct ActiveConfigLocation {
    std::string configDir;
    std::string sharedThemePath;    // ~/.config/nythera/theme.toml when running on split config layout
    std::string legacySettingsPath; // non-empty only when using unmigrated legacy config and settings.toml exists
    bool usingLegacy = false;
  };
  [[nodiscard]] static ActiveConfigLocation resolveActiveConfigLocation();
  [[nodiscard]] static std::string resolveActiveStateDir();

  // Read Shell settings without persisting migrations. App state
  // remains writable through the existing owner-scoped state API.
  // InheritanceOnly opens the Shell StateStore read-only without creating directories or tightening permissions.
  enum class SettingsAccess { ReadOnly, ReadWrite, InheritanceOnly };
  explicit ConfigService(SettingsAccess access);
  ~ConfigService() = default;

  ConfigService(const ConfigService&) = delete;
  ConfigService& operator=(const ConfigService&) = delete;

  [[nodiscard]] const std::string& activeConfigDir() const noexcept { return m_configDir; }
  [[nodiscard]] const std::string& sharedThemeConfigPath() const noexcept { return m_sharedThemePath; }
  [[nodiscard]] const nythera::config::ConfigSourceMap& sourceMap() const noexcept { return m_sourceMap; }
  [[nodiscard]] const Config& config() const noexcept { return m_config; }
  [[nodiscard]] std::shared_ptr<const toml::table> configTable() const noexcept { return m_configTable; }
  [[nodiscard]] bool isLockScreenEnabled() const noexcept { return ::isLockScreenEnabled(m_config.lockscreen); }
  [[nodiscard]] bool shouldLockBeforeSuspend() const noexcept { return ::shouldLockBeforeSuspend(m_config.lockscreen); }
  // Which sections changed in the reload currently being dispatched. Valid while
  // reload callbacks run; subscribers consult it to skip unaffected work.
  [[nodiscard]] const ConfigChangeSet& lastChange() const noexcept { return m_lastChange; }
  [[nodiscard]] const std::string& lastMutationError() const noexcept { return m_lastMutationError; }
  [[nodiscard]] bool matchesKeybind(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers) const;
  [[nodiscard]] int watchFd() const noexcept { return m_inotify.fd(); }
  [[nodiscard]] std::string buildSupportReport() const;
  [[nodiscard]] std::string buildMergedUserConfig(nythera::config::CredentialExportMode credentials) const;
  [[nodiscard]] std::string buildEffectiveConfig(nythera::config::CredentialExportMode credentials) const;
  [[nodiscard]] static std::string buildMergedUserConfigFromSources(
      std::string_view configDir, std::string_view settingsPath, std::string* error = nullptr,
      nythera::config::CredentialExportMode credentials = nythera::config::CredentialExportMode::Keep,
      std::string_view sharedThemePath = {}
  );
  [[nodiscard]] static std::string buildEffectiveConfigFromSources(
      std::string_view configDir, std::string_view settingsPath, std::string* error = nullptr,
      nythera::config::CredentialExportMode credentials = nythera::config::CredentialExportMode::Keep,
      std::string_view sharedThemePath = {}
  );
  [[nodiscard]] bool shouldRunSetupWizard() const;
  [[nodiscard]] std::optional<bool> stateBool(std::string_view owner, std::string_view key) const;
  [[nodiscard]] bool stateOwnerValid(std::string_view owner) const;
  [[nodiscard]] bool stateContains(std::string_view owner, std::string_view key) const;
  [[nodiscard]] const std::string& stateParseError() const noexcept { return m_stateStore.parseError(); }
  [[nodiscard]] std::optional<std::vector<std::string>>
  stateStringArray(std::string_view owner, std::string_view key) const;
  bool setStateStringArray(std::string_view owner, std::string_view key, const std::vector<std::string>& value);
  [[nodiscard]] std::optional<std::string> stateString(std::string_view owner, std::string_view key) const;
  [[nodiscard]] const nythera::config::LegacyConfigIssues& legacyConfigIssues() const noexcept {
    return m_legacyConfigIssues;
  }

  // The optional label is used only for opt-in reload profiling (NYTHERA_PROFILE);
  // unlabeled subscribers are reported by registration index.
  void addReloadCallback(ReloadCallback callback, std::string_view label = {});
  void setNotificationManager(ConfigNotificationSink* sink);
  void checkReload();
  void forceReload();

  void registerIpc(IpcService& ipc);

  // Persisted wallpaper paths (written directly to user configuration).
  [[nodiscard]] std::string getWallpaperPath(const std::string& connectorName) const;
  [[nodiscard]] const std::unordered_map<std::string, std::string>& monitorWallpaperPaths() const {
    return m_monitorWallpaperPaths;
  }
  [[nodiscard]] std::string getDefaultWallpaperPath() const;
  // Last applied wallpaper, else default. Drives palette generation and template previews.
  [[nodiscard]] std::string getPaletteWallpaperPath() const;
  // Greeter sync fallback when no monitor-specific path is chosen.
  [[nodiscard]] std::string getGreeterSyncWallpaperPath() const;
  void setWallpaperPath(const std::optional<std::string>& connectorName, const std::string& path);
  void setWallpaperChangeCallback(ChangeCallback callback);

  [[nodiscard]] const std::vector<WallpaperFavorite>& wallpaperFavorites() const noexcept;
  [[nodiscard]] bool isWallpaperFavorite(std::string_view path) const;
  [[nodiscard]] const WallpaperFavorite* wallpaperFavorite(std::string_view path) const;
  void addWallpaperFavorite(std::string path, const std::optional<WallpaperFavorite>& preset = std::nullopt);
  void removeWallpaperFavorite(std::string_view path);
  void setWallpaperFavoriteThemeMode(std::string_view path, ThemeMode themeMode);
  void setWallpaperFavoritePaletteSource(std::string_view path, std::optional<PaletteSource> source);
  void setWallpaperFavoritePaletteSelection(std::string_view path, std::string_view value);
  void applyWallpaperSelection(
      const std::optional<std::string>& connectorName, const std::string& path, const WallpaperFavorite* applyTheme,
      const std::vector<std::string>& allConnectors
  );

  // Add/remove a plugin id ("author/plugin") from the effective [plugins].enabled
  // list. Persists directly to user configuration and triggers the reload pipeline.
  // No-op if already in that state.
  void setPluginEnabled(std::string_view pluginId, bool enabled);

  // Add (replacing any same-named entry) or remove a plugin source in
  // [[plugins.source]], then trigger the reload pipeline.
  void addPluginSource(const PluginSourceConfig& source);
  void removePluginSource(std::string_view name);

  // Persist the global [plugins].auto_update mode directly and trigger the
  // reload pipeline. Drives background auto-update of git sources per mode.
  void setPluginsAutoUpdate(PluginAutoUpdateMode mode);

  // Persist theme-mode directly and trigger the reload pipeline.
  void setThemeMode(ThemeMode mode);
  // Persist `[theme].source` and the palette field for that source, then reload.
  [[nodiscard]] bool setThemeColorScheme(PaletteSource source, std::string_view value);
  // Persist dock enabled directly and trigger the reload pipeline.
  void setDockEnabled(bool enabled);
  // Persist desktop/lockscreen widget layout/editor state directly and trigger the reload pipeline.
  bool setDesktopWidgetsState(const DesktopWidgetsConfig& desktopWidgets);
  bool setLockscreenWidgetsState(const LockscreenWidgetsConfig& lockscreenWidgets);
  // Persist app-owned UI/runtime state to state.toml. This does not affect Config reloads.
  bool setStateBool(std::string_view owner, std::string_view key, bool value);
  bool setStateString(std::string_view owner, std::string_view key, std::string_view value);
  bool clearStateOwner(std::string_view owner);
  bool markSetupWizardCompleted();
  [[nodiscard]] bool hasOverride(const std::vector<std::string>& path) const;
  [[nodiscard]] bool hasEffectiveOverride(const std::vector<std::string>& path) const;
  // Lane content as the settings GUI presents it: the widget list plus the capsule groups it
  // references, so moving a widget into a lane's group still reports the lane as overridden.
  [[nodiscard]] bool hasEffectiveBarLaneOverride(const std::vector<std::string>& lanePath) const;
  // Reverts a lane to the config file: drops the lane-list override and restores the capsule groups
  // that lane references, leaving groups owned by other lanes alone.
  bool resetBarLaneOverride(const std::vector<std::string>& lanePath, bool* changed = nullptr);
  [[nodiscard]] bool isOverrideOnlyBar(std::string_view name) const;
  [[nodiscard]] bool isOverrideOnlyCalendarAccount(std::string_view id) const;
  [[nodiscard]] bool canMoveBarOverride(std::string_view name, int direction) const;
  [[nodiscard]] bool canDeleteBarOverride(std::string_view name) const;
  [[nodiscard]] bool isOverrideOnlyMonitorOverride(std::string_view barName, std::string_view match) const;
  [[nodiscard]] bool isOverrideOnlyDockMonitorOverride(std::string_view tableName) const;
  bool createBarOverride(std::string_view name);
  bool moveBarOverride(std::string_view name, int direction);
  bool renameBarOverride(std::string_view oldName, std::string_view newName);
  bool deleteBarOverride(std::string_view name);
  bool createMonitorOverride(std::string_view barName, std::string_view match);
  bool renameMonitorOverride(std::string_view barName, std::string_view oldMatch, std::string_view newMatch);
  bool deleteMonitorOverride(std::string_view barName, std::string_view match);
  bool createDockMonitorOverride(std::string_view match);
  bool renameDockMonitorOverride(std::string_view oldTableName, std::string_view newMatch);
  bool deleteDockMonitorOverride(std::string_view tableName);
  bool deleteCalendarAccountOverride(std::string_view id);
  bool setOverride(const std::vector<std::string>& path, ConfigOverrideValue value);
  bool setOverride(const std::vector<std::string>& path, ConfigOverrideValue value, bool* changed);
  [[nodiscard]] bool validateOverride(
      const std::vector<std::string>& path, const ConfigOverrideValue& value, std::string* error = nullptr
  );
  bool setOverrides(const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides);
  bool
  setOverrides(const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides, bool* changed);
  // Sets and clears in one commit. Callers that both write new keys and retire keys the new shape no
  // longer owns must use this: two separate commits publish an intermediate config that fails schema
  // validation and raises a config-error notification.
  bool mutateOverrides(
      const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides,
      const std::vector<std::vector<std::string>>& clearPaths, bool* changed
  );
  bool clearOverride(const std::vector<std::string>& path);
  bool clearOverrides(const std::vector<std::vector<std::string>>& paths, bool* changed);
  bool renameOverrideTable(const std::vector<std::string>& oldPath, const std::vector<std::string>& newPath);
  bool renameOverrideTable(
      const std::vector<std::string>& oldPath, const std::vector<std::string>& newPath,
      const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& extraOverrides
  );

  [[nodiscard]] static BarConfig resolveForOutput(const BarConfig& base, const WaylandOutput& output);
  [[nodiscard]] static DockConfig resolveForOutput(const DockConfig& base, const WaylandOutput& output);

  // Recursively overlays `overlay` onto `base` (tables merge, everything else
  // replaces). Public so `config validate` can reproduce loadAll's merge order.
  static void deepMerge(toml::table& base, const toml::table& overlay);
  // Serializes a toml++ node to a single TOML value expression. Public so Shell
  // mutation helpers in config_mutations.cpp can format DirectEdit payloads.
  [[nodiscard]] static std::string formatTomlValue(const toml::node& node);

private:
  struct DirectEdit {
    nythera::config::ConfigKeyPath path;
    std::optional<std::string> value; // nullopt removes across all contributing files (or destination if set)
    std::optional<std::filesystem::path> destination;
  };

  struct WatchEntry {
    std::filesystem::path directory;
    bool scanToml = false;
    std::unordered_set<std::string> names;
  };

  // A config problem kept as location + text rather than one pre-joined string:
  // the on-screen notification puts the location in its title and the text in
  // its body. `message` is "<dotted.path>: <problem>" and never carries a location.
  struct ConfigProblem {
    nythera::config::schema::SourceOrigin origin;
    std::string message;

    [[nodiscard]] bool empty() const { return message.empty(); }
    // Single-line form for logs and the settings status banner.
    [[nodiscard]] std::string flatten(std::string_view baseDir) const { return origin.prefixedShort(baseDir, message); }
    [[nodiscard]] static ConfigProblem from(const nythera::config::schema::Diagnostics::Entry& entry) {
      return ConfigProblem{entry.origin, entry.path + ": " + entry.message};
    }
  };

  void loadAll();
  void applyPendingWatchReload();
  [[nodiscard]] static Config makeDefaultConfig();
  static void
  parseConfigTable(const toml::table& tbl, Config& config, bool logSummary, bool logSchemaDiagnostics = true);
  [[nodiscard]] std::filesystem::path defaultDestinationForPath(const nythera::config::ConfigKeyPath& path) const;
  [[nodiscard]] bool persistDirectEdits(
      std::span<const DirectEdit> edits, bool* changed = nullptr, bool rejectWarnings = false,
      bool reloadOnSuccess = true
  );
  [[nodiscard]] bool overlayLegacySidecar(
      toml::table& merged, nythera::config::ConfigOriginIndex& origins, toml::table& sidecar, bool& hasSidecar,
      ConfigProblem* errorOut = nullptr
  ) const;
  [[nodiscard]] nythera::config::schema::Diagnostics diagnosticsForTable(
      const toml::table& merged, const nythera::config::ConfigOriginIndex& origins,
      const toml::table* legacySidecar = nullptr
  ) const;
  [[nodiscard]] std::optional<Config>
  parseMergedConfig(const toml::table& normalized, const toml::table* legacySidecar = nullptr) const;
  [[nodiscard]] bool pathEffectiveInUserTable(
      const std::vector<std::string>& path, const toml::table& userTable, const Config* parsedWith = nullptr
  ) const;
  [[nodiscard]] static std::size_t
  preserveDepthForPath(const std::vector<std::string>& path, const toml::table& userTable);
  void updateWatches();
  void fireReloadCallbacks();
  void setConfigParseError(ConfigProblem problem);
  void updateLegacyConfigIssues(nythera::config::LegacyConfigIssues issues);
  void notifyLegacyConfigIssues();
  void extractWallpaperFromTable(const toml::table& table);
  [[nodiscard]] bool hasConfiguredWallpaper() const;
  [[nodiscard]] std::string firstRunWallpaperPath() const;

  Config m_config;
  ConfigService(SettingsAccess access, MergedConfigValidator validator, OverrideEffectivenessChecker effectiveness);

  SettingsAccess m_settingsAccess = SettingsAccess::ReadWrite;
  MergedConfigValidator m_validator;
  OverrideEffectivenessChecker m_effectivenessChecker;
  ConfigChangeSet m_lastChange;

  std::shared_ptr<const toml::table> m_configTable;

  // Authoritative Shell config directory (~/.config/nythera/shell, or legacy ~/.config/nythera while unmigrated).
  std::string m_configDir;
  // Shared global theme configuration (~/.config/nythera/theme.toml when not in legacy mode).
  std::string m_sharedThemePath;
  // Read-only legacy state-dir settings.toml overlay when running on unmigrated live configuration.
  std::string m_legacySettingsPath;
  bool m_usingLegacyConfigDir = false;
  nythera::config::ConfigSourceMap m_sourceMap;
  toml::table m_mergedUserTable;

  // App-owned UI/runtime state. This is not a config layer and is never
  // deep-merged into Config.
  StateStore m_stateStore;
  // Marker file (state dir): its existence means onboarding has been completed
  // or dismissed. Single canonical signal for the setup wizard.
  std::string m_setupMarkerPath;
  std::string m_defaultWallpaperPath;
  std::string m_lastWallpaperPath;
  std::unordered_map<std::string, std::string> m_monitorWallpaperPaths;
  std::vector<WallpaperFavorite> m_wallpaperFavorites;
  mutable std::unordered_map<std::string, bool> m_effectiveOverrideCache;

  ConfigProblem m_pendingError;             // parse error from initial load, notified once the manager is wired up
  uint32_t m_configErrorNotificationId = 0; // ID of the active config-error notification, 0 if none
  nythera::config::LegacyConfigIssues m_legacyConfigIssues;
  std::string m_loggedLegacyIssueFingerprint;
  bool m_legacyReminderPending = false;
  Timer m_legacyReminderTimer;
  ConfigNotificationSink* m_notificationManager = nullptr;

  Inotify m_inotify;
  std::map<int, WatchEntry> m_watches;
  Timer m_watchReloadTimer;
  bool m_pendingConfigReload = false;
  std::vector<std::filesystem::path> m_includeLoadedFiles;
  std::vector<std::filesystem::path> m_includeDirs;
  std::vector<std::filesystem::path> m_watchPaths;
  std::map<std::filesystem::path, std::string> m_selfWrites;

  std::string m_lastMutationError;
  int m_wallpaperBatchDepth = 0;
  bool m_wallpaperBatchDirty = false;

  ChangeCallback m_wallpaperChangeCallback;
  struct ReloadSubscriber {
    ReloadCallback callback;
    std::string label;
  };
  std::vector<ReloadSubscriber> m_reloadCallbacks;
};
