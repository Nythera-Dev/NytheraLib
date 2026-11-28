#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace filemanager {

  enum class FileColumn { Name, Size, Modified, Type, Created, Accessed };

  inline constexpr std::array kFileColumns{FileColumn::Name, FileColumn::Size,    FileColumn::Modified,
                                           FileColumn::Type, FileColumn::Created, FileColumn::Accessed};
  inline constexpr std::array kDefaultFileColumns{FileColumn::Name, FileColumn::Size, FileColumn::Modified};
  inline constexpr std::array<std::string_view, kFileColumns.size()> kFileColumnLabels{"Name", "Size",    "Modified",
                                                                                       "Type", "Created", "Accessed"};
  // Search uses the Created cell for Location. The shared row keeps its existing layout.
  inline constexpr FileColumn kSearchLocationSlot = FileColumn::Created;
  inline constexpr auto kSearchFileColumnLabels = [] {
    auto labels = kFileColumnLabels;
    labels[static_cast<std::size_t>(kSearchLocationSlot)] = "Location";
    return labels;
  }();
  inline constexpr float fileColumnTextInset(bool trailing, float gap) noexcept {
    return trailing ? 12.0F : 12.0F - gap * 0.5F;
  }

  inline constexpr float kMetadataFontSizeIncrease = 2.0F;

  inline constexpr bool fileColumnUsesTabularNumerals(FileColumn column, bool searchView) noexcept {
    return column == FileColumn::Size
        || column == FileColumn::Modified
        || column == FileColumn::Accessed
        || (column == FileColumn::Created && !searchView);
  }

  inline constexpr std::size_t columnIndex(FileColumn column) noexcept { return static_cast<std::size_t>(column); }

  // Logical pixels, keyed by identity so moving or hiding a column retains its width.
  struct FileColumns {
    std::array<float, kFileColumns.size()> widths{0.0F, 96.0F, 152.0F, 144.0F, 152.0F, 152.0F};

    float& operator[](FileColumn column) noexcept { return widths[columnIndex(column)]; }
    float operator[](FileColumn column) const noexcept { return widths[columnIndex(column)]; }
  };

  inline constexpr FileColumns kMinimumFileColumns{{120.0F, 72.0F, 112.0F, 88.0F, 112.0F, 112.0F}};

  struct FileColumnLayout {
    std::array<FileColumn, kFileColumns.size()> order = kFileColumns;
    std::array<bool, kFileColumns.size()> visible{true, true, true, false, false, false};

    [[nodiscard]] std::vector<FileColumn> displayed() const {
      std::vector<FileColumn> columns;
      for (auto column : order)
        if (visible[columnIndex(column)])
          columns.push_back(column);
      return columns;
    }

    // Move through the visible sequence, leaving hidden slots in place.
    bool move(FileColumn column, std::size_t destination) {
      auto columns = displayed();
      const auto from = std::ranges::find(columns, column);
      if (from == columns.end() || destination >= columns.size() || from == columns.begin() + destination)
        return false;
      columns.erase(from);
      columns.insert(columns.begin() + destination, column);
      auto next = columns.begin();
      for (auto& slot : order)
        if (visible[columnIndex(slot)])
          slot = *next++;
      return true;
    }
  };

  inline constexpr FileColumnLayout kDefaultSearchColumnLayout{
      {FileColumn::Name, kSearchLocationSlot, FileColumn::Size, FileColumn::Modified, FileColumn::Type,
       FileColumn::Accessed},
      {true, true, true, true, true, false},
  };

  inline float fileColumnTotal(FileColumns widths, std::span<const FileColumn> visible) {
    float total = 0.0F;
    for (auto column : visible)
      total += widths[column];
    return total;
  }

  inline bool hasOptionalFileColumns(std::span<const FileColumn> visible) {
    return std::ranges::any_of(visible, [](auto column) { return column >= FileColumn::Type; });
  }

  inline FileColumns
  fitFileColumns(FileColumns widths, float available, std::span<const FileColumn> visible = kDefaultFileColumns) {
    available = std::max(0.0F, available);
    const bool flexibleName = widths[FileColumn::Name] <= 0.0F;
    for (auto column : visible)
      widths[column] = std::max(kMinimumFileColumns[column], widths[column]);
    const float minimum = fileColumnTotal(kMinimumFileColumns, visible);
    const float wanted = fileColumnTotal(widths, visible);
    if (available < minimum) {
      for (auto column : visible)
        widths[column] = kMinimumFileColumns[column] * available / minimum;
    } else if (hasOptionalFileColumns(visible) && wanted > available) {
      // Keep newly shown columns in view without changing any saved widths.
      const float ratio = (available - minimum) / (wanted - minimum);
      for (auto column : visible)
        widths[column] = kMinimumFileColumns[column] + (widths[column] - kMinimumFileColumns[column]) * ratio;
    } else if (flexibleName && wanted < available) {
      widths[FileColumn::Name] += available - wanted;
    }
    return widths;
  }

  inline FileColumns resizeFileColumnBoundary(
      FileColumns widths, FileColumn column, float delta, float available,
      std::span<const FileColumn> visible = kDefaultFileColumns, bool flexibleName = false
  ) {
    widths = fitFileColumns(widths, available, visible);
    if (available < fileColumnTotal(kMinimumFileColumns, visible) || !std::isfinite(delta))
      return widths;
    const float original = widths[column];
    widths[column] = std::clamp(original + delta, kMinimumFileColumns[column], 10000.0F);
    if (hasOptionalFileColumns(visible)) {
      const auto name = FileColumn::Name;
      if (flexibleName && column != name)
        widths[name] = kMinimumFileColumns[name];
      const float excess = fileColumnTotal(widths, visible) - available;
      if (excess > 0.0F)
        widths[column] = std::max(kMinimumFileColumns[column], widths[column] - excess);
      if (flexibleName && column != name)
        widths[name] = 0.0F;
      widths = fitFileColumns(widths, available, visible);
    }
    return widths;
  }

} // namespace filemanager
