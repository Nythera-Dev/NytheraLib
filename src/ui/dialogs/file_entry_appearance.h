#pragma once

#include <string>

namespace filemanager {
  inline bool isHiddenFileName(const std::string& name) { return !name.empty() && name.front() == '.'; }

  inline constexpr float
  fileEntryIconNameAlpha(bool hidden, bool selected, bool hovered, bool disabled, bool cut = false) {
    return cut ? 0.5F : (disabled ? 0.55F : (hidden && !selected && !hovered ? 0.78F : 1.0F));
  }
} // namespace filemanager
