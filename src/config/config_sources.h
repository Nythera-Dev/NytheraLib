#pragma once

#include "config/schema/diagnostics.h"
#include "core/toml.h"

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nythera::config {

  // Each element is one decoded TOML key, including literal dots and empty keys.
  // Arrays (including session actions) are addressed as whole settings.
  using ConfigKeyPath = std::vector<std::string>;

  enum class ConfigDefinitionKind { Value, Table, Array };

  struct ConfigDefinition {
    schema::SourceOrigin origin;
    ConfigDefinitionKind kind;
  };

  class ConfigSourceMap {
  public:
    // Definitions in increasing merge precedence, including shadowed ones.
    // A table entry lists every file defining or extending that table. Paths
    // are absolute and retain the spelling through which each file was loaded.
    [[nodiscard]] std::span<const ConfigDefinition> definitions(const ConfigKeyPath& path) const;
    // The effective definition, or the highest-precedence contributor for a table.
    [[nodiscard]] const ConfigDefinition* winner(const ConfigKeyPath& path) const;

    // Record a freshly parsed file before toml++ copies discard positions.
    // The top-level loader directive [include] is excluded.
    void record(const std::filesystem::path& file, const toml::table& document);
    // Combine source maps at the same boundary as the corresponding TOML merge.
    void overlay(const ConfigSourceMap& source);

  private:
    struct Entry {
      std::vector<ConfigDefinition> definitions;
      std::optional<std::size_t> winner;
    };

    void recordNode(ConfigKeyPath& path, const toml::node& node, const std::string& file);
    std::map<ConfigKeyPath, Entry> m_entries;
  };

} // namespace nythera::config
