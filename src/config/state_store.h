#pragma once

#include "core/toml.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class StateStore {
public:
  StateStore() = default;
  explicit StateStore(std::filesystem::path path);

  void setPath(std::filesystem::path path);
  void load();
  void loadReadOnly();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
  [[nodiscard]] const std::string& parseError() const noexcept { return m_parseError; }
  [[nodiscard]] std::optional<bool> boolValue(std::string_view owner, std::string_view key) const;
  [[nodiscard]] std::optional<std::string> stringValue(std::string_view owner, std::string_view key) const;
  [[nodiscard]] std::unordered_map<std::string, std::string>
  stringMap(std::string_view owner, std::string_view subtable) const;

  [[nodiscard]] bool ownerValid(std::string_view owner) const;
  [[nodiscard]] bool contains(std::string_view owner, std::string_view key) const;
  [[nodiscard]] std::optional<std::vector<std::string>>
  stringArrayValue(std::string_view owner, std::string_view key) const;
  bool setStringArray(std::string_view owner, std::string_view key, const std::vector<std::string>& value);

  bool setBool(std::string_view owner, std::string_view key, bool value);
  bool setString(std::string_view owner, std::string_view key, std::string_view value);
  bool setValues(
      std::string_view owner, std::span<const std::pair<std::string_view, std::string_view>> stringEntries,
      std::string_view mapSubtable = {}, std::span<const std::pair<std::string_view, std::string_view>> mapEntries = {}
  );
  bool clearOwner(std::string_view owner);

private:
  [[nodiscard]] bool write();

  std::filesystem::path m_path;
  toml::table m_state;
  std::string m_parseError;
};
