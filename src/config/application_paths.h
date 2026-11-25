#pragma once

#include "util/file_utils.h"

namespace FileUtils {

  enum class Application { Shell, Files };

  [[nodiscard]] inline std::string applicationConfigDir(Application application) {
    const std::string base = configDir();
    return base.empty() ? base : base + (application == Application::Shell ? "/shell" : "/files");
  }

  [[nodiscard]] inline std::string applicationStateDir(Application application) {
    const std::string base = stateDir();
    return base.empty() ? base : base + (application == Application::Shell ? "/shell" : "/files");
  }

} // namespace FileUtils
