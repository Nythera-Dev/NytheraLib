#include "core/files/resource_paths.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <dlfcn.h>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace paths {

  namespace {

    constexpr Logger kLog("paths");
    bool s_usingFilesAssets = false;

    struct InstallPaths {
      std::filesystem::path prefix{NYTHERA_INSTALL_PREFIX};
      std::filesystem::path datadir{NYTHERA_INSTALL_DATADIR};
    };

    InstallPaths& installPaths() {
      static InstallPaths paths;
      return paths;
    }

    std::filesystem::path joinInstalledSubdir(
        const std::filesystem::path& prefix, const std::filesystem::path& datadir, std::string_view appDir,
        std::string_view bundleDir
    ) {
      if (datadir.is_absolute()) {
        return datadir / appDir / bundleDir;
      }
      return prefix / datadir / appDir / bundleDir;
    }

    std::filesystem::path
    installedAssetsRoot(const std::filesystem::path& prefix, const std::filesystem::path& datadir) {
      return joinInstalledSubdir(prefix, datadir, "nythera", "assets");
    }

    std::filesystem::path
    installedFilesAssetsRoot(const std::filesystem::path& prefix, const std::filesystem::path& datadir) {
      return joinInstalledSubdir(prefix, datadir, "nythera", "files-assets");
    }

    std::filesystem::path
    installedSharedAssetsRoot(const std::filesystem::path& prefix, const std::filesystem::path& datadir) {
      return joinInstalledSubdir(prefix, datadir, "nythera-lib", "assets");
    }

    bool isAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      std::error_code ec;
      return std::filesystem::exists(root / "emoji.json", ec)
          && std::filesystem::exists(root / "templates" / "builtin.toml", ec)
          && std::filesystem::exists(root / "translations" / "en.json", ec);
    }

    bool isFilesAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      std::error_code ec;
      return std::filesystem::is_regular_file(root / "dialogue-window-artwork" / "top-left-diagonals.svg", ec)
          && std::filesystem::is_regular_file(root / "templates" / "filemanager" / "office" / "blank.odt", ec);
    }

    bool isSharedAssetRoot(const std::filesystem::path& root) {
      if (root.empty()) {
        return false;
      }

      std::error_code ec;
      return std::filesystem::is_regular_file(root / "fonts" / "NytheraDisplay-Regular.ttf", ec)
          && std::filesystem::is_regular_file(root / "fonts" / "nythera-tabler.ttf", ec)
          && std::filesystem::is_regular_file(root / "fonts" / "tabler.json", ec)
          && std::filesystem::is_regular_file(root / "translations" / "en.json", ec);
    }

    std::optional<std::filesystem::path> executablePath() {
      std::array<char, 4096> buffer{};
      const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
      if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1) {
        return std::nullopt;
      }

      buffer[static_cast<std::size_t>(count)] = '\0';
      return std::filesystem::path(buffer.data());
    }

    std::optional<std::filesystem::path> libraryPath() {
      Dl_info info{};
      if (::dladdr(reinterpret_cast<const void*>(&sharedAssetsRoot), &info) == 0
          || info.dli_fname == nullptr
          || info.dli_fname[0] == '\0') {
        return std::nullopt;
      }
      std::error_code ec;
      const std::filesystem::path candidate(info.dli_fname);
      const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
      return (!ec && !resolved.empty()) ? std::optional<std::filesystem::path>(resolved)
                                        : std::optional<std::filesystem::path>(candidate);
    }

    std::string envString(const char* name) {
      const char* value = std::getenv(name);
      return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string{};
    }

    void appendUnique(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate) {
      if (candidate.empty()) {
        return;
      }
      if (!std::ranges::contains(candidates, candidate)) {
        candidates.push_back(candidate);
      }
    }

    std::vector<std::filesystem::path> shellAssetCandidates(const AssetResolutionContext& context) {
      std::vector<std::filesystem::path> candidates;

      if (!context.executablePath.empty()) {
        const std::filesystem::path exeDir = context.executablePath.parent_path();
        appendUnique(candidates, exeDir / "assets");
        appendUnique(candidates, exeDir.parent_path() / "assets");

        if (!context.installDatadir.empty() && !context.installDatadir.is_absolute()) {
          appendUnique(candidates, exeDir.parent_path() / context.installDatadir / "nythera" / "assets");
        }
        appendUnique(candidates, exeDir.parent_path() / "share" / "nythera" / "assets");
      }

      appendUnique(candidates, installedAssetsRoot(context.installPrefix, context.installDatadir));
      return candidates;
    }

    std::vector<std::filesystem::path> filesAssetCandidates(const AssetResolutionContext& context) {
      std::vector<std::filesystem::path> candidates;

      if (!context.executablePath.empty()) {
        const std::filesystem::path exeDir = context.executablePath.parent_path();
        appendUnique(candidates, exeDir / "files-assets");

        if (!context.installDatadir.empty() && !context.installDatadir.is_absolute()) {
          appendUnique(candidates, exeDir.parent_path() / context.installDatadir / "nythera" / "files-assets");
        }
        appendUnique(candidates, exeDir.parent_path() / "share" / "nythera" / "files-assets");
      }

      appendUnique(candidates, installedFilesAssetsRoot(context.installPrefix, context.installDatadir));
      return candidates;
    }

    std::vector<std::filesystem::path> sharedAssetCandidates(const SharedAssetResolutionContext& context) {
      std::vector<std::filesystem::path> candidates;

      if (!context.libraryPath.empty()) {
        const std::filesystem::path libDir = context.libraryPath.parent_path();
        const std::filesystem::path parentDir = libDir.parent_path();
        const std::filesystem::path grandParentDir = parentDir.parent_path();

        if (!context.installDatadir.empty() && !context.installDatadir.is_absolute()) {
          appendUnique(candidates, parentDir / context.installDatadir / "nythera-lib" / "assets");
          appendUnique(candidates, grandParentDir / context.installDatadir / "nythera-lib" / "assets");
        }
        appendUnique(candidates, parentDir / "share" / "nythera-lib" / "assets");
        appendUnique(candidates, grandParentDir / "share" / "nythera-lib" / "assets");
        appendUnique(candidates, libDir / "assets");
        appendUnique(candidates, parentDir / "assets");
      }

      appendUnique(candidates, installedSharedAssetsRoot(context.installPrefix, context.installDatadir));
      return candidates;
    }

    std::filesystem::path resolveRuntimeAssetsRoot() {
      const AssetResolutionContext context{
          .envOverride = envString("NYTHERA_ASSETS_DIR"),
          .executablePath = executablePath().value_or(std::filesystem::path{}),
          .installPrefix = installPaths().prefix,
          .installDatadir = installPaths().datadir,
      };
      return s_usingFilesAssets ? resolveFilesAssetsRoot(context) : resolveShellAssetsRoot(context);
    }

    std::filesystem::path resolveRuntimeSharedAssetsRoot() {
      const SharedAssetResolutionContext context{
          .envOverride = envString("NYTHERA_LIB_ASSETS_DIR"),
          .libraryPath = libraryPath().value_or(std::filesystem::path{}),
          .installPrefix = std::filesystem::path(NYTHERA_LIB_INSTALL_PREFIX),
          .installDatadir = std::filesystem::path(NYTHERA_LIB_INSTALL_DATADIR),
      };
      return resolveSharedAssetsRoot(context);
    }

  } // namespace

  std::filesystem::path resolveShellAssetsRoot(const AssetResolutionContext& context) {
    if (!context.envOverride.empty()) {
      const std::filesystem::path overridePath(context.envOverride);
      if (isAssetRoot(overridePath)) {
        kLog.debug("using assets from {}", overridePath.string());
        return overridePath;
      }
      kLog.warn("NYTHERA_ASSETS_DIR is not a valid asset bundle: {}", overridePath.string());
    }

    for (const auto& candidate : shellAssetCandidates(context)) {
      if (isAssetRoot(candidate)) {
        kLog.debug("using assets from {}", candidate.string());
        return candidate;
      }
    }

    const std::filesystem::path fallback = installedAssetsRoot(context.installPrefix, context.installDatadir);
    kLog.warn("could not locate a valid asset bundle; defaulting to {}", fallback.string());
    return fallback;
  }

  std::filesystem::path resolveFilesAssetsRoot(const AssetResolutionContext& context) {
    if (!context.envOverride.empty()) {
      const std::filesystem::path overridePath(context.envOverride);
      if (isFilesAssetRoot(overridePath)) {
        kLog.debug("using Files assets from {}", overridePath.string());
        return overridePath;
      }
      kLog.warn("NYTHERA_ASSETS_DIR is not a valid Files asset bundle: {}", overridePath.string());
    }

    for (const auto& candidate : filesAssetCandidates(context)) {
      if (isFilesAssetRoot(candidate)) {
        kLog.debug("using Files assets from {}", candidate.string());
        return candidate;
      }
    }

    const std::filesystem::path fallback = installedFilesAssetsRoot(context.installPrefix, context.installDatadir);
    kLog.warn("could not locate a valid Files asset bundle; defaulting to {}", fallback.string());
    return fallback;
  }

  std::filesystem::path resolveSharedAssetsRoot(const SharedAssetResolutionContext& context) {
    if (!context.envOverride.empty()) {
      const std::filesystem::path overridePath(context.envOverride);
      if (isSharedAssetRoot(overridePath)) {
        kLog.debug("using NytheraLib assets from {}", overridePath.string());
        return overridePath;
      }
      kLog.warn("NYTHERA_LIB_ASSETS_DIR is not a valid NytheraLib asset bundle: {}", overridePath.string());
    }

    for (const auto& candidate : sharedAssetCandidates(context)) {
      if (isSharedAssetRoot(candidate)) {
        kLog.debug("using NytheraLib assets from {}", candidate.string());
        return candidate;
      }
    }

    const std::filesystem::path fallback = installedSharedAssetsRoot(context.installPrefix, context.installDatadir);
    kLog.warn("could not locate a valid NytheraLib asset bundle; defaulting to {}", fallback.string());
    return fallback;
  }

  void setInstallPaths(std::string_view prefix, std::string_view datadir) {
    installPaths() = {std::filesystem::path(prefix), std::filesystem::path(datadir)};
  }

  void selectFilesAssets() noexcept { s_usingFilesAssets = true; }

  bool usingFilesAssets() noexcept { return s_usingFilesAssets; }

  const std::filesystem::path& assetsRoot() {
    static const std::filesystem::path s_root = resolveRuntimeAssetsRoot();
    return s_root;
  }

  std::filesystem::path assetPath(std::string_view relativePath) {
    return assetsRoot() / std::filesystem::path(std::string(relativePath));
  }

  const std::filesystem::path& sharedAssetsRoot() {
    static const std::filesystem::path s_root = resolveRuntimeSharedAssetsRoot();
    return s_root;
  }

  std::filesystem::path sharedAssetPath(std::string_view relativePath) {
    return sharedAssetsRoot() / std::filesystem::path(std::string(relativePath));
  }

} // namespace paths
