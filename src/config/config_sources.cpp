#include "config/config_sources.h"

#include <algorithm>

namespace nythera::config {

  std::span<const ConfigDefinition> ConfigSourceMap::definitions(const ConfigKeyPath& path) const {
    const auto entry = m_entries.find(path);
    if (entry == m_entries.end()) {
      return {};
    }
    return entry->second.definitions;
  }

  const ConfigDefinition* ConfigSourceMap::winner(const ConfigKeyPath& path) const {
    const auto entry = m_entries.find(path);
    if (entry == m_entries.end() || !entry->second.winner) {
      return nullptr;
    }
    return &entry->second.definitions[*entry->second.winner];
  }

  void ConfigSourceMap::record(const std::filesystem::path& file, const toml::table& document) {
    ConfigSourceMap source;
    const std::string filename = std::filesystem::absolute(file).string();
    ConfigKeyPath path;
    for (const auto& [key, node] : document) {
      if (key != "include") {
        path.assign(1, std::string(key.str()));
        source.recordNode(path, node, filename);
      }
    }
    overlay(source);
  }

  void ConfigSourceMap::recordNode(ConfigKeyPath& path, const toml::node& node, const std::string& file) {
    const auto& position = node.source().begin;
    const auto* table = node.as_table();
    const auto kind = table != nullptr ? ConfigDefinitionKind::Table
        : node.is_array()              ? ConfigDefinitionKind::Array
                                       : ConfigDefinitionKind::Value;
    m_entries.emplace(path, Entry{{ConfigDefinition{{file, position.line, position.column}, kind}}, 0});
    if (table != nullptr) {
      for (const auto& [key, child] : *table) {
        path.emplace_back(key.str());
        recordNode(path, child, file);
        path.pop_back();
      }
    }
  }

  void ConfigSourceMap::overlay(const ConfigSourceMap& source) {
    const ConfigKeyPath* ignoredActions = nullptr;
    std::vector<const ConfigKeyPath*> mergedSessions;
    for (const auto& [path, incoming] : source.m_entries) {
      auto& entry = m_entries[path];
      const std::size_t offset = entry.definitions.size();
      entry.definitions.append_range(incoming.definitions);
      if (ignoredActions && !std::ranges::starts_with(path, *ignoredActions)) {
        ignoredActions = nullptr;
      }
      while (!mergedSessions.empty() && !std::ranges::starts_with(path, *mergedSessions.back())) {
        mergedSessions.pop_back();
      }
      if (!incoming.winner || ignoredActions) {
        continue;
      }

      const auto kind = incoming.definitions[*incoming.winner].kind;
      // deepMerge only applies array-valued actions when both session nodes are tables.
      if (!mergedSessions.empty()
          && path.size() == mergedSessions.back()->size() + 1
          && path.back() == "actions"
          && kind != ConfigDefinitionKind::Array) {
        ignoredActions = &path;
        continue;
      }
      if (path.back() == "session"
          && kind == ConfigDefinitionKind::Table
          && entry.winner
          && entry.definitions[*entry.winner].kind == ConfigDefinitionKind::Table) {
        mergedSessions.push_back(&path);
      }

      entry.winner = offset + *incoming.winner;
      if (kind != ConfigDefinitionKind::Table) {
        // A scalar or array replaces a whole table. Keep its old definitions
        // for reset/deletion, while removing their status as effective sources.
        for (auto child = m_entries.upper_bound(path);
             child != m_entries.end() && std::ranges::starts_with(child->first, path); ++child) {
          child->second.winner.reset();
        }
      }
    }
  }

} // namespace nythera::config
