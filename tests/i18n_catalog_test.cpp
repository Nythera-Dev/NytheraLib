#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "test_check.h"
#include "test_isolation.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace {

  void writeText(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    TEST_CHECK(out.is_open());
    out << content;
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    // 1. Validate NytheraLib's own 29 locale catalogs against kSupportedLanguages.
    const char* rawLibAssets = std::getenv("NYTHERA_LIB_ASSETS_DIR");
    TEST_CHECK(rawLibAssets != nullptr && rawLibAssets[0] != '\0');
    const std::filesystem::path libTranslations = std::filesystem::path(rawLibAssets) / "translations";
    TEST_CHECK(std::filesystem::is_directory(libTranslations));

    std::set<std::string> supported;
    for (const auto& language : i18n::kSupportedLanguages) {
      supported.insert(std::string(language.code));
      const std::filesystem::path catalogPath = libTranslations / (std::string(language.code) + ".json");
      TEST_CHECK(std::filesystem::is_regular_file(catalogPath));
      std::ifstream in(catalogPath);
      TEST_CHECK(in.is_open());
      const auto parsed = nlohmann::json::parse(in);
      TEST_CHECK(parsed.is_object());
    }

    for (const auto& entry : std::filesystem::directory_iterator(libTranslations)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") {
        continue;
      }
      TEST_CHECK(supported.contains(entry.path().stem().string()));
    }

    // 2. Validate layered shared + app loading, precedence, fallback, interpolation, and pluralisation.
    nythera::test::ScopedTempDir temp("nythera-i18n-catalog");
    const auto sharedDir = temp.path() / "shared";
    const auto appDir = temp.path() / "app";
    const auto missingAppDir = temp.path() / "no-app-translations";

    writeText(
        sharedDir / "en.json",
        R"({
          "shared": {
            "only": "Shared English",
            "override": "Shared Value",
            "item": "{count} item",
            "item-plural": "{count} items",
            "greeting": "Hello {name}"
          }
        })"
    );
    writeText(
        sharedDir / "fr.json",
        R"({
          "shared": {
            "override": "Valeur Partagée",
            "item": "{count} élément",
            "item-plural": "{count} éléments"
          }
        })"
    );
    writeText(
        appDir / "en.json",
        R"({
          "shared": {
            "override": "App English Override"
          },
          "app": {
            "only": "App English",
            "fallback": "App English Fallback"
          }
        })"
    );
    writeText(
        appDir / "fr.json",
        R"({
          "shared": {
            "override": "Valeur App"
          },
          "app": {
            "only": "App Français"
          }
        })"
    );

    auto& service = i18n::Service::instance();

    // Shared-only mode (e.g. Files without app translations).
    service.init("fr", sharedDir, missingAppDir);
    TEST_CHECK(service.language() == "fr");
    TEST_CHECK(service.lookup("shared.override") == "Valeur Partagée");
    TEST_CHECK(service.lookup("shared.only") == "Shared English");
    TEST_CHECK(i18n::trp("shared.item", 1) == "1 élément");
    TEST_CHECK(i18n::trp("shared.item", 3) == "3 éléments");
    TEST_CHECK(i18n::tr("shared.greeting", "name", "Nythera") == "Hello Nythera");
    TEST_CHECK(service.lookup("app.only").empty());

    // Layered Shared + App mode: app wins on collision, missing active keys fall back to English in both layers.
    service.init("fr", sharedDir, appDir);
    TEST_CHECK(service.language() == "fr");
    TEST_CHECK(service.lookup("shared.override") == "Valeur App");
    TEST_CHECK(service.lookup("shared.only") == "Shared English");
    TEST_CHECK(service.lookup("app.only") == "App Français");
    TEST_CHECK(service.lookup("app.fallback") == "App English Fallback");

    // Switching language via setLanguage preserves the configured directories.
    service.setLanguage("en");
    TEST_CHECK(service.language() == "en");
    TEST_CHECK(service.lookup("shared.override") == "App English Override");
    TEST_CHECK(service.lookup("app.only") == "App English");

    // Unknown locale falls back cleanly to merged English.
    service.setLanguage("zz-Nonexistent");
    TEST_CHECK(service.language() == "en");
    TEST_CHECK(service.lookup("shared.only") == "Shared English");
    TEST_CHECK(service.lookup("app.only") == "App English");
  });
}
