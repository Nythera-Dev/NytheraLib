#include "config/toml_persistence.h"
#include "test_isolation.h"

#include <array>
#include <fstream>
#include <iterator>
#include <print>

namespace {

  namespace fs = std::filesystem;
  using namespace nythera::config;

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    TEST_CHECK(input);
    return {std::istreambuf_iterator<char>(input), {}};
  }

  void write(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    TEST_CHECK(output << text);
  }

  void checkSuccess(const TomlPersistenceResult& result) {
    if (!result.ok()) {
      std::println(stderr, "Persistence failed: {}", result.error);
    }
    TEST_CHECK(result.ok());
  }

  void checkFormattingAndSymlinks(const nythera::test::ScopedTestEnv& env) {
    const auto target = env.work() / "external.toml";
    const auto link = env.config() / "config.toml";
    const auto relativeTarget = fs::path("../work/external.toml");
    write(target, R"(# Header

[monitors."DP.1"]
# Resolution
scale = 1.0 # User scale
mask = 0x12_ab
name = 'desk'
unchanged = 'literal'
nested = { original = true }
)");
    fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    const auto mode = fs::status(target).permissions();
    fs::create_symlink(relativeTarget, link);

    const std::array edits{
        TomlFileEdits{
            link,
            {{{"monitors", "DP.1", "scale"}, "1.25"},
             {{"monitors", "DP.1", "name"}, R"("updated")"},
             {{"monitors", "DP.1", "mask"}, "4781"}}
        },
        TomlFileEdits{
            target, {{{"monitors", "DP.1", "nested", "new", "key"}, "true"}, {{"new", "section", "items"}, "[]"}}
        },
    };
    checkSuccess(persistTomlEdits(edits));
    TEST_CHECK(fs::read_symlink(link) == relativeTarget);
    TEST_CHECK(fs::status(target).permissions() == mode);
    const auto content = read(target);
    TEST_CHECK(content.contains("# Header") && content.contains("# Resolution") && content.contains("# User scale"));
    TEST_CHECK(
        content.contains("mask = 0x12_ad")
        && content.contains("name = 'updated'")
        && content.contains("unchanged = 'literal'")
    );
    TEST_CHECK(content.find("scale =") < content.find("mask ="));
    const auto parsed = toml::parse(content);
    TEST_CHECK(parsed["monitors"]["DP.1"]["scale"].value<double>() == 1.25);
    TEST_CHECK(parsed["monitors"]["DP.1"]["nested"]["new"]["key"].value<bool>() == true);
    TEST_CHECK(parsed["new"]["section"]["items"].as_array()->empty());
    checkSuccess(persistTomlEdits(edits));
    TEST_CHECK(read(target) == content);

    // Adding a scalar or inline array of tables under an implicit parent table promotes it
    // cleanly while preserving comments.
    const auto implicitParent = env.config() / "implicit-parent.toml";
    write(implicitParent, "# Wallpaper header\n[wallpaper.default]\npath = '/a.png'\n");
    checkSuccess(persistTomlEdits(
        std::array{TomlFileEdits{
            implicitParent,
            {{{"wallpaper", "favorite"}, R"([ { path = "/b.png", theme_mode = "light" } ])"},
             {{"wallpaper", "shuffle"}, "true"}},
        }}
    ));
    const auto implicitContent = read(implicitParent);
    TEST_CHECK(implicitContent.contains("# Wallpaper header"));
    const auto implicitParsed = toml::parse(implicitContent);
    TEST_CHECK(implicitParsed["wallpaper"]["default"]["path"].value<std::string>() == "/a.png");
    TEST_CHECK(implicitParsed["wallpaper"]["shuffle"].value<bool>() == true);
    TEST_CHECK(implicitParsed["wallpaper"]["favorite"][0]["path"].value<std::string>() == "/b.png");
    TEST_CHECK(implicitParsed["wallpaper"]["favorite"][0]["theme_mode"].value<std::string>() == "light");

    const auto dangling = env.config() / "user-theme.toml";
    fs::create_symlink("../work/new-theme.toml", dangling);
    checkSuccess(persistTomlEdits(std::array{TomlFileEdits{dangling, {{{"theme", "mode"}, "'dark'"}}}}));
    TEST_CHECK(fs::is_symlink(dangling));
    TEST_CHECK(
        toml::parse_file((env.work() / "new-theme.toml").string())["theme"]["mode"].value<std::string>() == "dark"
    );
  }

  void checkValidationAndRollback(const nythera::test::ScopedTestEnv& env) {
    const auto first = env.config() / "first.toml";
    const auto newFile = env.config() / "created.toml";
    const auto second = env.work() / "second.toml";
    constexpr auto original = "key  = 1 # preserved on failure\n";
    write(first, original);
    write(second, "broken = [\n");
    fs::permissions(first, fs::perms::owner_read | fs::perms::owner_write);
    const auto originalMode = fs::status(first).permissions();
    const std::array edits{
        TomlFileEdits{first, {{{"key"}, "2"}}},
        TomlFileEdits{newFile, {{{"key"}, "2"}}},
        TomlFileEdits{second, {{{"key"}, "2"}}},
    };
    TEST_CHECK(!persistTomlEdits(edits).ok());
    TEST_CHECK(read(first) == original && !fs::exists(newFile));

    write(second, original);
    const auto rejected = persistTomlEdits(edits, [&](std::span<const StagedTomlFile> files) {
      TEST_CHECK(files.size() == 3 && files.front().path == first);
      TEST_CHECK(toml::parse(files.front().content)["key"].value<int>() == 2);
      TEST_CHECK(read(first) == original && !fs::exists(newFile));
      return std::string("test schema rejection");
    });
    TEST_CHECK(rejected.error == "test schema rejection");
    TEST_CHECK(read(first) == original && read(second) == original && !fs::exists(newFile));

    // Read-only parent still permits staging/reading, but makes the last atomic
    // replacement fail after both earlier files have been written.
    const auto mode = fs::status(env.work()).permissions();
    fs::permissions(env.work(), fs::perms::owner_read | fs::perms::owner_exec);
    const auto restored = persistTomlEdits(edits);
    fs::permissions(env.work(), mode);
    TEST_CHECK(restored.error.contains("atomic replacement failed"));
    TEST_CHECK(restored.rollbackFailures.empty());
    TEST_CHECK(read(first) == original && read(second) == original && !fs::exists(newFile));
    TEST_CHECK(fs::status(first).permissions() == originalMode);

    fs::permissions(second, fs::perms::owner_read);
    const auto readOnly = persistTomlEdits(edits);
    TEST_CHECK(readOnly.error.contains("not writable"));
    TEST_CHECK(read(first) == original && read(second) == original && !fs::exists(newFile));
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    const nythera::test::ScopedTestEnv env({.prefix = "nythera-toml-persistence"});
    checkFormattingAndSymlinks(env);
    checkValidationAndRollback(env);
  });
}
