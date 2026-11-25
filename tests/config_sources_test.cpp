#include "config/config_merge.h"
#include "config/toml_persistence.h"
#include "test_isolation.h"

#include <fstream>

namespace {

  namespace fs = std::filesystem;
  using namespace nythera::config;

  void write(const fs::path& path, std::string_view content) {
    std::ofstream output(path);
    TEST_CHECK(output << content);
  }

  void checkSourcesAndWriteBack(const nythera::test::ScopedTestEnv& env) {
    const auto config = env.config();
    const auto early = config / "00-base.toml";
    const auto host = config / "90-host.toml";
    const auto external = env.work() / "shared.toml";
    write(early, R"(
[bar."DP.1"]
size = 10
early = true
[bar.DP.1]
size = 40
[shell.session]
actions = [{ action = "lock", label = "Lock" }, { action = "suspend" }]
)");
    write(external, R"(
[bar."DP.1"]
size = 20
external = true
[shell.session]
actions = [{ action = "lock", icon = "lock" }]
)");
    write(host, R"(
[include]
files = ["../work/shared.toml"]
[bar."DP.1"]
size = 30
host = true
[shell.session]
actions = "ignored"
)");

    const auto loaded = mergeConfigWithSources(config.string());
    const auto legacy = mergeConfigWithIncludes(config.string());
    TEST_CHECK(loaded.config.firstError.empty());
    TEST_CHECK(loaded.config.merged == legacy.merged);
    TEST_CHECK(loaded.config.loadedFiles == legacy.loadedFiles);
    TEST_CHECK(loaded.config.includeDirs == legacy.includeDirs);
    const ConfigKeyPath size{"bar", "DP.1", "size"};
    const auto definitions = loaded.sources.definitions(size);
    TEST_CHECK(definitions.size() == 3);
    TEST_CHECK(definitions[0].origin.file == early && definitions[0].origin.line == 3);
    TEST_CHECK(fs::equivalent(definitions[1].origin.file, external));
    TEST_CHECK(definitions[2].origin.file == host);
    TEST_CHECK(loaded.sources.winner(size) == &definitions.back());
    TEST_CHECK(loaded.sources.definitions({"bar", "DP", "1", "size"}).size() == 1);
    TEST_CHECK(loaded.sources.definitions({"bar", "DP.1"}).size() == 3);
    TEST_CHECK(loaded.sources.winner({"bar", "DP.1", "early"})->origin.file == early);
    TEST_CHECK(loaded.sources.definitions({"include"}).empty());

    const auto actions = loaded.config.merged["shell"]["session"]["actions"];
    TEST_CHECK(actions.as_array() && actions.as_array()->size() == 1);
    TEST_CHECK(actions[0]["label"].value<std::string>() == "Lock");
    TEST_CHECK(actions[0]["icon"].value<std::string>() == "lock");
    const ConfigKeyPath actionsPath{"shell", "session", "actions"};
    TEST_CHECK(loaded.sources.definitions(actionsPath).size() == 3);
    TEST_CHECK(fs::equivalent(loaded.sources.winner(actionsPath)->origin.file, external));

    // Stage 1 callers can target the winner or every definition without changing
    // the loader or requiring external includes to live under the config root.
    const std::vector<TomlFileEdits> setWinner{{loaded.sources.winner(size)->origin.file, {{size, "31"}}}};
    TEST_CHECK(persistTomlEdits(setWinner).ok());
    TEST_CHECK(toml::parse_file(host.string())["bar"]["DP.1"]["size"].value<int>() == 31);

    std::vector<TomlFileEdits> reset;
    for (const auto& definition : definitions) {
      reset.push_back({definition.origin.file, {{size, std::nullopt}}});
    }
    TEST_CHECK(persistTomlEdits(reset).ok());
    const auto afterReset = mergeConfigWithSources(config.string());
    TEST_CHECK(afterReset.sources.definitions(size).empty());
    TEST_CHECK(afterReset.config.merged["bar"]["DP"]["1"]["size"].value<int>() == 40);

    const ConfigKeyPath entity{"bar", "DP.1"};
    std::vector<TomlFileEdits> removeEntity;
    for (const auto& definition : afterReset.sources.definitions(entity)) {
      removeEntity.push_back({definition.origin.file, {{entity, std::nullopt}}});
    }
    TEST_CHECK(persistTomlEdits(removeEntity).ok());
    TEST_CHECK(!mergeConfigWithSources(config.string()).config.merged["bar"]["DP.1"]);
  }

  void checkGroupBoundaries(const nythera::test::ScopedTestEnv& env) {
    const auto config = env.work() / "grouped";
    TEST_CHECK(fs::create_directory(config));
    const auto early = config / "00-base.toml";
    const auto host = config / "90-host.toml";
    const auto hidden = env.work() / "hidden.toml";
    const auto barrier = env.work() / "barrier.toml";
    write(early, "[settings]\nretained = 1\n");
    write(hidden, "[settings]\nretained = 2\nshadowed = 3\n");
    write(barrier, "settings = 4\n");
    write(host, "[include]\nfiles = ['../hidden.toml', '../barrier.toml']\n[settings]\nvisible = 5\n");

    const ConfigKeyPath retained{"settings", "retained"};
    const ConfigKeyPath shadowed{"settings", "shadowed"};
    const auto loaded = mergeConfigWithSources(config.string());
    TEST_CHECK(loaded.config.merged["settings"]["retained"].value<int>() == 1);
    TEST_CHECK(loaded.sources.definitions(retained).size() == 2);
    TEST_CHECK(loaded.sources.winner(retained)->origin.file == early);
    TEST_CHECK(loaded.sources.definitions(shadowed).size() == 1);
    TEST_CHECK(loaded.sources.winner(shadowed) == nullptr);

    write(host, "[include]\nautoload = false\nfiles = ['../hidden.toml']\n[settings]\nvisible = 5\n");
    const auto optedOut = mergeConfigWithSources(config.string());
    TEST_CHECK(optedOut.config.loadedFiles.size() == 2);
    TEST_CHECK(optedOut.config.merged["settings"]["retained"].value<int>() == 2);
    TEST_CHECK(optedOut.sources.definitions(retained).size() == 1);
    TEST_CHECK(fs::equivalent(optedOut.sources.winner(retained)->origin.file, hidden));
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-config-sources"});
    checkSourcesAndWriteBack(env);
    checkGroupBoundaries(env);
  });
}
