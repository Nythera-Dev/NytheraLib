#include "core/files/resource_paths.h"
#include "test_check.h"
#include "test_isolation.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

  void writeEmptyFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    TEST_CHECK(out.is_open());
    out << "{}\n";
  }

  void createSharedBundle(const std::filesystem::path& root) {
    writeEmptyFile(root / "fonts" / "NytheraDisplay-Regular.ttf");
    writeEmptyFile(root / "fonts" / "nythera-tabler.ttf");
    writeEmptyFile(root / "fonts" / "tabler.json");
    writeEmptyFile(root / "translations" / "en.json");
  }

  void createShellBundle(const std::filesystem::path& root) {
    writeEmptyFile(root / "emoji.json");
    writeEmptyFile(root / "templates" / "builtin.toml");
    writeEmptyFile(root / "translations" / "en.json");
  }

  void createFilesBundle(const std::filesystem::path& root) {
    writeEmptyFile(root / "dialogue-window-artwork" / "top-left-diagonals.svg");
    writeEmptyFile(root / "templates" / "filemanager" / "office" / "blank.odt");
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTempDir temp("nythera-resource-paths");
    const auto& root = temp.path();

    // 1. Files asset resolution: env override, dev bundle, exe-relative installed/relocated, fallback, and
    //    rejection of obsolete monorepo paths & sibling bundles.
    {
      const auto layout = root / "files-relocated";
      const auto exe = layout / "usr" / "bin" / "nythera-file-manager";
      const auto devBundle = layout / "usr" / "bin" / "files-assets";
      const auto installedRelocated = layout / "usr" / "share" / "nythera" / "files-assets";
      const auto customDatadirBundle = layout / "usr" / "share-custom" / "nythera" / "files-assets";
      const auto compileFallback = layout / "compile-prefix" / "share" / "nythera" / "files-assets";
      const auto envOverride = layout / "env-override";

      // Obsolete monorepo paths plus Shell/Lib bundles must never match as Files assets.
      createShellBundle(layout / "usr" / "bin" / "assets");
      createShellBundle(layout / "usr" / "assets");
      createSharedBundle(layout / "usr" / "bin" / "assets");
      createSharedBundle(layout / "usr" / "assets");
      createFilesBundle(compileFallback);

      // Compile-time fallback when no exe-relative candidate exists.
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .executablePath = exe,
              .installPrefix = layout / "compile-prefix",
              .installDatadir = "share",
          })
          == compileFallback
      );

      // Absolute datadir compile-time fallback.
      const auto absDatadirFallback = layout / "abs-data" / "nythera" / "files-assets";
      createFilesBundle(absDatadirFallback);
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .executablePath = exe,
              .installPrefix = layout / "unused-prefix",
              .installDatadir = layout / "abs-data",
          })
          == absDatadirFallback
      );

      // Executable-relative ../share/nythera/files-assets takes precedence over compile-time fallback.
      createFilesBundle(installedRelocated);
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .executablePath = exe,
              .installPrefix = layout / "compile-prefix",
              .installDatadir = "share",
          })
          == installedRelocated
      );

      // Executable-relative ../<datadir>/nythera/files-assets when datadir is custom relative path.
      createFilesBundle(customDatadirBundle);
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .executablePath = exe,
              .installPrefix = layout / "compile-prefix",
              .installDatadir = "share-custom",
          })
          == customDatadirBundle
      );

      // Development bundle beside executable (<exeDir>/files-assets) wins over installed paths.
      createFilesBundle(devBundle);
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .executablePath = exe,
              .installPrefix = layout / "compile-prefix",
              .installDatadir = "share",
          })
          == devBundle
      );

      // Invalid NYTHERA_ASSETS_DIR falls back cleanly to exe-relative discovery; valid override wins.
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .envOverride = (layout / "missing").string(),
              .executablePath = exe,
              .installPrefix = layout / "compile-prefix",
              .installDatadir = "share",
          })
          == devBundle
      );

      createFilesBundle(envOverride);
      TEST_CHECK(
          paths::resolveFilesAssetsRoot({
              .envOverride = envOverride.string(),
              .executablePath = exe,
              .installPrefix = layout / "compile-prefix",
              .installDatadir = "share",
          })
          == envOverride
      );
    }

    // 2. Shell asset resolution stays isolated from Files/Lib bundles and works without fonts/ in Shell assets.
    {
      const auto layout = root / "shell-relocated";
      const auto exe = layout / "staged" / "bin" / "nythera";
      const auto installedShell = layout / "staged" / "share" / "nythera" / "assets";
      createFilesBundle(layout / "staged" / "bin" / "files-assets");
      createSharedBundle(layout / "staged" / "bin" / "assets");
      createShellBundle(installedShell);

      TEST_CHECK(
          paths::resolveShellAssetsRoot({
              .executablePath = exe,
              .installPrefix = layout / "other-prefix",
              .installDatadir = "share",
          })
          == installedShell
      );
    }

    // 3. Shared NytheraLib asset resolution: env override, lib-relative installed (including multiarch), dev, fallback.
    {
      const auto layout = root / "lib-relocated";
      const auto libSo = layout / "opt" / "nythera-lib" / "lib" / "libnythera_lib.so.0";
      const auto libInstalled = layout / "opt" / "nythera-lib" / "share" / "nythera-lib" / "assets";
      const auto libCompileFallback = layout / "compile-lib" / "share" / "nythera-lib" / "assets";
      const auto libOverride = layout / "lib-override";

      // Shell and Files bundles must never satisfy shared asset discovery.
      createShellBundle(layout / "opt" / "nythera-lib" / "share" / "nythera-lib" / "assets");
      createFilesBundle(libCompileFallback);

      const auto cleanCompileFallback = layout / "clean-compile-lib" / "share" / "nythera-lib" / "assets";
      createSharedBundle(cleanCompileFallback);
      TEST_CHECK(
          paths::resolveSharedAssetsRoot({
              .libraryPath = layout / "nowhere" / "libnythera_lib.so.0",
              .installPrefix = layout / "clean-compile-lib",
              .installDatadir = "share",
          })
          == cleanCompileFallback
      );

      // Installed lib-relative ../share/nythera-lib/assets.
      createSharedBundle(libInstalled);
      TEST_CHECK(
          paths::resolveSharedAssetsRoot({
              .libraryPath = libSo,
              .installPrefix = layout / "clean-compile-lib",
              .installDatadir = "share",
          })
          == libInstalled
      );

      // Multiarch lib/<arch>/libnythera_lib.so -> ../../share/nythera-lib/assets.
      const auto multiarchPrefix = layout / "multiarch";
      const auto multiarchLib = multiarchPrefix / "lib" / "x86_64-linux-gnu" / "libnythera_lib.so.0";
      const auto multiarchAssets = multiarchPrefix / "share" / "nythera-lib" / "assets";
      createSharedBundle(multiarchAssets);
      TEST_CHECK(
          paths::resolveSharedAssetsRoot({
              .libraryPath = multiarchLib,
              .installPrefix = layout / "clean-compile-lib",
              .installDatadir = "share",
          })
          == multiarchAssets
      );

      // Development build layout (<repo>/build-debug/libnythera_lib.so -> <repo>/assets).
      const auto devRepo = layout / "dev-repo";
      const auto devLib = devRepo / "build-debug" / "libnythera_lib.so";
      const auto devAssets = devRepo / "assets";
      createSharedBundle(devAssets);
      TEST_CHECK(
          paths::resolveSharedAssetsRoot({
              .libraryPath = devLib,
              .installPrefix = layout / "clean-compile-lib",
              .installDatadir = "share",
          })
          == devAssets
      );

      // Explicit NYTHERA_LIB_ASSETS_DIR override wins when valid; invalid override falls back.
      TEST_CHECK(
          paths::resolveSharedAssetsRoot({
              .envOverride = (layout / "missing").string(),
              .libraryPath = libSo,
              .installPrefix = layout / "clean-compile-lib",
              .installDatadir = "share",
          })
          == libInstalled
      );

      createSharedBundle(libOverride);
      TEST_CHECK(
          paths::resolveSharedAssetsRoot({
              .envOverride = libOverride.string(),
              .libraryPath = libSo,
              .installPrefix = layout / "clean-compile-lib",
              .installDatadir = "share",
          })
          == libOverride
      );
    }
  });
}
