#pragma once

#include "config/config_sources.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nythera::config {

  struct TomlEdit {
    ConfigKeyPath path;
    // A single TOML value expression, or nullopt to remove the key and its subtree.
    std::optional<std::string> value;
  };

  struct TomlFileEdits {
    std::filesystem::path path;
    std::vector<TomlEdit> edits;
  };

  struct StagedTomlFile {
    std::filesystem::path path; // Resolved write target.
    std::string content;
    std::vector<std::filesystem::path> aliases; // Requested paths before symlink resolution.
  };

  struct TomlPersistenceResult {
    std::string error;
    // Targets that could not be restored after a partial write.
    std::vector<std::filesystem::path> rollbackFailures;

    [[nodiscard]] bool ok() const { return error.empty(); }
  };

  using TomlWriteValidator = std::function<std::optional<std::string>(std::span<const StagedTomlFile>)>;

  // Applies ordered edits to named files, staging and parsing every changed document
  // before the first write. A validator may reject the staged batch. Each file is
  // replaced atomically, with best-effort rollback if a later replacement fails.
  // The caller chooses the destinations using its configuration ownership rules.
  [[nodiscard]] TomlPersistenceResult
  persistTomlEdits(std::span<const TomlFileEdits> files, const TomlWriteValidator& validate = {});

} // namespace nythera::config
