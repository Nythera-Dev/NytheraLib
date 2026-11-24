#include "i18n/i18n_service.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include "i18n/language_tag.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace i18n {

  namespace {

    constexpr Logger kLog("i18n");

    void flatten(const nlohmann::json& node, const std::string& prefix, Catalog& out) {
      for (auto it = node.begin(); it != node.end(); ++it) {
        std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
          flatten(*it, path, out);
        } else if (it->is_string()) {
          out.insert_or_assign(std::move(path), it->get<std::string>());
        }
      }
    }

    // Reads $LC_ALL / $LC_MESSAGES / $LANG. Returns empty string if unset.
    std::string detectSystemLanguage() {
      for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (value != nullptr && value[0] != '\0') {
          return detail::normalizeLanguageTag(value);
        }
      }
      return {};
    }

    std::string resolveRequestedLanguage(std::string_view preferredLang) {
      return preferredLang.empty() ? detectSystemLanguage() : detail::normalizeLanguageTag(preferredLang);
    }

    bool loadCatalogFile(const std::filesystem::path& path, Catalog& out) {
      std::ifstream file(path);
      if (!file.is_open()) {
        return false;
      }
      try {
        auto json = nlohmann::json::parse(file);
        if (!json.is_object()) {
          kLog.warn("catalog {} is not a JSON object", path.string());
          return false;
        }
        flatten(json, {}, out);
        return true;
      } catch (const std::exception& e) {
        kLog.error("failed to parse {}: {}", path.string(), e.what());
        return false;
      }
    }

    bool sameDirectory(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
      if (lhs.empty() || rhs.empty()) {
        return false;
      }
      std::error_code ec;
      if (std::filesystem::exists(lhs, ec) && std::filesystem::exists(rhs, ec)) {
        const bool same = std::filesystem::equivalent(lhs, rhs, ec);
        if (!ec) {
          return same;
        }
      }
      return lhs.lexically_normal() == rhs.lexically_normal();
    }

  } // namespace

  Service& Service::instance() {
    static Service s_instance;
    return s_instance;
  }

  bool Service::loadCatalog(
      std::string_view lang, const std::filesystem::path& sharedDir, const std::filesystem::path& appDir, Catalog& out
  ) const {
    Catalog merged;
    bool loaded = false;

    if (!sharedDir.empty()) {
      const std::filesystem::path sharedPath = sharedDir / (std::string(lang) + ".json");
      if (loadCatalogFile(sharedPath, merged)) {
        loaded = true;
      }
    }

    if (!appDir.empty() && !sameDirectory(sharedDir, appDir)) {
      const std::filesystem::path appPath = appDir / (std::string(lang) + ".json");
      Catalog appCatalog;
      if (loadCatalogFile(appPath, appCatalog)) {
        for (auto& [key, value] : appCatalog) {
          if (const auto existing = merged.find(key); existing != merged.end() && existing->second != value) {
            kLog.warn("application catalog {} overrides shared key '{}'", appPath.string(), key);
          }
          merged.insert_or_assign(key, std::move(value));
        }
        loaded = true;
      }
    }

    if (loaded) {
      out = std::move(merged);
    }
    return loaded;
  }

  void Service::init(std::string_view preferredLang) {
    init(preferredLang, paths::sharedAssetPath("translations"), paths::assetPath("translations"));
  }

  void Service::init(
      std::string_view preferredLang, const std::filesystem::path& sharedTranslationsDir,
      const std::filesystem::path& appTranslationsDir
  ) {
    m_sharedDir = sharedTranslationsDir;
    m_appDir = appTranslationsDir;
    m_active.clear();
    m_fallback.clear();

    const std::string candidate = resolveRequestedLanguage(preferredLang);
    m_requestedLanguage = candidate;

    // English fallback is always loaded first so lookup() can fall back to it
    // even if the active catalog is English itself.
    if (!loadCatalog("en", m_sharedDir, m_appDir, m_fallback)) {
      kLog.warn("could not load English fallback catalog");
    }

    if (candidate.empty() || candidate == "en") {
      m_active = m_fallback;
      m_language = "en";
      m_rtl = false;
      kLog.info("language: en");
      return;
    }

    for (const std::string& lang : detail::catalogLanguageCandidates(candidate)) {
      if (loadCatalog(lang, m_sharedDir, m_appDir, m_active)) {
        m_language = lang;
        m_rtl = detail::isRtlLanguage(m_language);
        if (lang == candidate) {
          kLog.info("language: {}", m_language);
        } else {
          kLog.info("language: {} (from {})", m_language, candidate);
        }
        return;
      }
    }

    kLog.warn("no catalog for '{}', falling back to English", candidate);
    m_active = m_fallback;
    m_language = "en";
    m_rtl = false;
  }

  void Service::setLanguage(std::string_view lang) {
    // Compare the resolved request, not the loaded catalog: an empty (auto) or regional tag
    // resolves to a different catalog name and would otherwise reload on every config reload.
    if (resolveRequestedLanguage(lang) == m_requestedLanguage) {
      return;
    }
    if (!m_sharedDir.empty() || !m_appDir.empty()) {
      const auto sharedDir = m_sharedDir;
      const auto appDir = m_appDir;
      init(lang, sharedDir, appDir);
    } else {
      init(lang);
    }
  }

  std::string_view Service::lookup(std::string_view dottedKey) const {
    if (auto it = m_active.find(dottedKey); it != m_active.end()) {
      return it->second;
    }
    if (auto it = m_fallback.find(dottedKey); it != m_fallback.end()) {
      return it->second;
    }
    return {};
  }

} // namespace i18n
