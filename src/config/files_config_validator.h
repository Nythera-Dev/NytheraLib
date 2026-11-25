#pragma once

#include "config/config_monitor.h"

#include <optional>

namespace nythera::config {

  // Validates a merged Nythera Files configuration (~/.config/nythera/files).
  [[nodiscard]] std::optional<ConfigurationProblem> validateFilesConfiguration(const SourcedMergeResult& loaded);

} // namespace nythera::config
