#include "config/application_paths.h"
#include "test_isolation.h"

namespace {

  void checkPaths(const std::filesystem::path& config, const std::filesystem::path& state) {
    using FileUtils::Application;
    TEST_CHECK(FileUtils::configDir() == config);
    TEST_CHECK(FileUtils::stateDir() == state);
    TEST_CHECK(FileUtils::applicationConfigDir(Application::Shell) == config / "shell");
    TEST_CHECK(FileUtils::applicationConfigDir(Application::Files) == config / "files");
    TEST_CHECK(FileUtils::sharedThemeConfigPath() == config / "theme.toml");
    TEST_CHECK(FileUtils::applicationStateDir(Application::Shell) == state / "shell");
    TEST_CHECK(FileUtils::applicationStateDir(Application::Files) == state / "files");
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    using nythera::test::ScopedEnvVar;
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-application-paths"});
    const ScopedEnvVar xdgConfig("XDG_CONFIG_HOME", (env.root() / "xdg-config").string());
    const ScopedEnvVar xdgState("XDG_STATE_HOME", (env.root() / "xdg-state").string());
    checkPaths(env.config() / "nythera", env.state() / "nythera");

    const ScopedEnvVar configOverride("NYTHERA_CONFIG_HOME", nullptr);
    const ScopedEnvVar stateOverride("NYTHERA_STATE_HOME", "");
    checkPaths(env.root() / "xdg-config/nythera", env.root() / "xdg-state/nythera");

    const ScopedEnvVar noXdgConfig("XDG_CONFIG_HOME", "");
    const ScopedEnvVar noXdgState("XDG_STATE_HOME", nullptr);
    checkPaths(env.home() / ".config/nythera", env.home() / ".local/state/nythera");

    const ScopedEnvVar noHome("HOME", nullptr);
    TEST_CHECK(FileUtils::applicationConfigDir(FileUtils::Application::Shell).empty());
    TEST_CHECK(FileUtils::applicationStateDir(FileUtils::Application::Files).empty());
    TEST_CHECK(FileUtils::sharedThemeConfigPath().empty());
  });
}
