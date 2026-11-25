// [theme].mode is the app-facing mode: it is what the resolved callback carries to templates,
// the GTK color scheme, and theme_mode_changed hooks. [theme].shell_mode pins the palette
// Nythera itself renders with, without moving the app-facing mode.

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "net/http_client.h"
#include "tests/test_check.h"
#include "tests/test_isolation.h"
#include "theme/builtin_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/fixed_palette.h"
#include "theme/palette.h"
#include "theme/theme_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>

namespace {

  struct Resolution {
    std::string appliedMode; // mode handed to the resolved callback
    std::string mode;        // ThemeService::resolvedMode()
    std::string shellMode;   // ThemeService::resolvedShellMode()
    bool shellLight = false; // ThemeService::isLightMode()
    nythera::theme::GeneratedPalette generated;
  };

  Resolution resolve(const std::filesystem::path& configPath, std::string_view themeToml) {
    std::filesystem::create_directories(configPath.parent_path());
    {
      std::ofstream out(configPath, std::ios::trunc);
      TEST_CHECK(out << themeToml);
    }

    ConfigService config;
    HttpClient http;
    nythera::theme::ThemeService theme(config, http);

    Resolution result;
    theme.setResolvedCallback([&result](const nythera::theme::GeneratedPalette& generated, std::string_view mode) {
      result.appliedMode = std::string(mode);
      result.generated = generated;
    });
    theme.apply();
    for (auto& call : DeferredCall::takePending()) {
      call();
    }

    result.mode = std::string(theme.resolvedMode());
    result.shellMode = std::string(theme.resolvedShellMode());
    result.shellLight = theme.isLightMode();
    return result;
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    {
      const nythera::test::ScopedTestEnv env({.prefix = "nythera-theme-shell-mode"});
      const std::filesystem::path configPath = env.config() / "nythera" / "shell" / "config.toml";

      // follow: one mode drives Nythera and apps together.
      {
        const Resolution r = resolve(configPath, "[theme]\nmode = \"light\"\nshell_mode = \"follow\"\n");
        TEST_CHECK(r.mode == "light");
        TEST_CHECK(r.shellMode == "light");
        TEST_CHECK(r.shellLight);
        TEST_CHECK(r.appliedMode == "light");
      }

      // Pinned dark shell, light apps: only the shell palette is held back.
      {
        const Resolution r = resolve(configPath, "[theme]\nmode = \"light\"\nshell_mode = \"dark\"\n");
        TEST_CHECK(r.mode == "light");
        TEST_CHECK(r.shellMode == "dark");
        TEST_CHECK(!r.shellLight);
        TEST_CHECK(r.appliedMode == "light");
      }

      // The inverse pinning, so a light shell cannot leak into what apps are told.
      {
        const Resolution r = resolve(configPath, "[theme]\nmode = \"dark\"\nshell_mode = \"light\"\n");
        TEST_CHECK(r.mode == "dark");
        TEST_CHECK(r.shellMode == "light");
        TEST_CHECK(r.shellLight);
        TEST_CHECK(r.appliedMode == "dark");
      }

      // An unset shell_mode behaves like follow.
      {
        const Resolution r = resolve(configPath, "[theme]\nmode = \"dark\"\n");
        TEST_CHECK(r.mode == "dark");
        TEST_CHECK(r.shellMode == "dark");
        TEST_CHECK(!r.shellLight);
        TEST_CHECK(r.appliedMode == "dark");
      }
    }

    // Legacy compatibility: when ~/.config/nythera/shell/ has no TOML files, read-only
    // legacy state/nythera/settings.toml still resolves theme mode without modifying disk.
    {
      const nythera::test::ScopedTestEnv env({.prefix = "nythera-theme-shell-mode-legacy"});
      const std::filesystem::path legacySettings = env.state() / "nythera" / "settings.toml";
      constexpr std::string_view legacyToml = "[theme]\nmode = \"light\"\nshell_mode = \"dark\"\n";
      const Resolution r = resolve(legacySettings, legacyToml);
      TEST_CHECK(r.mode == "light");
      TEST_CHECK(r.shellMode == "dark");
      TEST_CHECK(!r.shellLight);
      TEST_CHECK(r.appliedMode == "light");

      std::ifstream in(legacySettings);
      TEST_CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == legacyToml);
    }

    // Shared ~/.config/nythera/theme.toml + ~/.config/nythera/palettes/, Shell override precedence,
    // and live inotify reload when theme.toml is created or updated externally.
    {
      const nythera::test::ScopedTestEnv env({.prefix = "nythera-shared-theme"});
      const std::filesystem::path nytheraDir = env.config() / "nythera";
      const std::filesystem::path sharedThemePath = nytheraDir / "theme.toml";
      const std::filesystem::path shellConfigPath = nytheraDir / "shell" / "config.toml";

      const Resolution sharedOnly = resolve(sharedThemePath, "[theme]\nmode = \"light\"\nshell_mode = \"dark\"\n");
      TEST_CHECK(!std::filesystem::exists(nytheraDir / "shell"));
      TEST_CHECK(sharedOnly.mode == "light");
      TEST_CHECK(sharedOnly.shellMode == "dark");
      TEST_CHECK(!sharedOnly.shellLight);

      const auto* ayu = nythera::theme::findBuiltinPalette("Ayu");
      const auto* nytheraDefault = nythera::theme::findBuiltinPalette("Nythera");
      TEST_CHECK(ayu != nullptr && nytheraDefault != nullptr);
      const nythera::theme::GeneratedPalette ayuGenerated = nythera::theme::expandBuiltinPalette(*ayu);
      TEST_CHECK(nythera::theme::saveCustomPaletteFromGenerated("shared-custom", ayuGenerated));
      TEST_CHECK(std::filesystem::exists(nytheraDir / "palettes" / "shared-custom.json"));

      const Resolution customRes = resolve(
          sharedThemePath,
          "[theme]\nmode = \"dark\"\nsource = \"custom\"\ncustom_palette = \"shared-custom\"\nbuiltin = \"Nythera\"\n"
      );
      TEST_CHECK(customRes.generated.dark.at("primary") == ayuGenerated.dark.at("primary"));
      TEST_CHECK(
          customRes.generated.dark.at("primary")
          != nythera::theme::expandBuiltinPalette(*nytheraDefault).dark.at("primary")
      );

      // Shell config under ~/.config/nythera/shell/ overrides shared theme.toml while keeping other keys.
      std::filesystem::create_directories(shellConfigPath.parent_path());
      {
        std::ofstream out(sharedThemePath, std::ios::trunc);
        TEST_CHECK(out << "[theme]\nmode = \"light\"\nshell_mode = \"dark\"\nbuiltin = \"Ayu\"\n");
      }
      {
        std::ofstream out(shellConfigPath, std::ios::trunc);
        TEST_CHECK(out << "[theme]\nshell_mode = \"light\"\n");
      }
      {
        ConfigService config;
        TEST_CHECK(config.config().theme.mode == ThemeMode::Light);
        TEST_CHECK(config.config().theme.shellMode == ShellThemeMode::Light);
        TEST_CHECK(config.config().theme.builtinPalette == "Ayu");
      }

      // Live reload from empty state when shared theme.toml is created externally.
      std::filesystem::remove_all(nytheraDir);
      ConfigService watched(ConfigService::SettingsAccess::InheritanceOnly);
      HttpClient http;
      nythera::theme::ThemeService theme(watched, http);
      std::string latestMode;
      theme.setResolvedCallback([&latestMode](const nythera::theme::GeneratedPalette&, std::string_view mode) {
        latestMode = std::string(mode);
      });
      theme.apply();
      for (auto& call : DeferredCall::takePending()) {
        call();
      }
      TEST_CHECK(latestMode == "dark");
      watched.addReloadCallback([&theme] { theme.onConfigReload(); }, "theme-test");

      std::filesystem::create_directories(nytheraDir);
      {
        std::ofstream out(sharedThemePath, std::ios::trunc);
        TEST_CHECK(out << "[theme]\nmode = \"light\"\nshell_mode = \"follow\"\n");
      }
      pollfd pfd{.fd = watched.watchFd(), .events = POLLIN, .revents = 0};
      TEST_CHECK(pfd.fd >= 0 && ::poll(&pfd, 1, 500) == 1 && (pfd.revents & POLLIN) != 0);
      watched.checkReload();
      std::this_thread::sleep_for(std::chrono::milliseconds(170));
      TimerManager::instance().tick();
      TEST_CHECK(watched.config().theme.mode == ThemeMode::Light);
      TEST_CHECK(theme.resolvedMode() == "light");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      TimerManager::instance().tick();
      for (auto& call : DeferredCall::takePending()) {
        call();
      }
      TEST_CHECK(latestMode == "light");
    }
  });
}
