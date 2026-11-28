#include "config/files_config_validator.h"

#include "config/config_types.h"
#include "core/files/directory_scanner.h"
#include "filemanager/file_columns.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace nythera::config {

  namespace {
    namespace fs = std::filesystem;

    bool validColumnWidths(std::string_view text) {
      std::size_t count = 0;
      while (count < filemanager::kFileColumns.size()) {
        const auto comma = text.find(',');
        const auto token = text.substr(0, comma);
        if (token.empty())
          return false;
        const auto index = count++;
        float value = 0.0F;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{}
            || end != token.data() + token.size()
            || !std::isfinite(value)
            || value < 0.0F
            || (value == 0.0F && index != 0)
            || value > 10000.0F)
          return false;
        if (comma == std::string_view::npos)
          return count == filemanager::kDefaultFileColumns.size() || count == filemanager::kFileColumns.size();
        text.remove_prefix(comma + 1);
      }
      return false;
    }

    bool validColumnLayout(std::string_view text) {
      std::array<bool, filemanager::kFileColumns.size()> seen{};
      for (std::size_t i = 0; i < filemanager::kFileColumns.size(); ++i) {
        const auto comma = text.find(',');
        auto token = text.substr(0, comma);
        const bool visible = !token.starts_with('-');
        if (!visible)
          token.remove_prefix(1);
        const auto found = std::ranges::find(filemanager::kFileColumnLabels, token);
        if (found == filemanager::kFileColumnLabels.end()
            || (comma == std::string_view::npos) != (i + 1 == filemanager::kFileColumns.size()))
          return false;
        const auto index = static_cast<std::size_t>(found - filemanager::kFileColumnLabels.begin());
        if (seen[index] || (!visible && filemanager::kFileColumns[index] == filemanager::FileColumn::Name))
          return false;
        seen[index] = true;
        if (comma != std::string_view::npos)
          text.remove_prefix(comma + 1);
      }
      return true;
    }

    bool validSearchColumnLayout(std::string_view text) {
      const auto location = text.find("Location");
      if (location == std::string_view::npos || text.contains("Created"))
        return false;
      std::string normalized(text);
      normalized.replace(location, sizeof("Location") - 1, "Created");
      if (!validColumnLayout(normalized))
        return false;
      return normalized.contains("-Accessed");
    }

    bool validPreference(std::string_view key, const toml::node& node) {
      if (key == "search_grid_view"
          || key == "search_sort_descending"
          || key == "grid_view"
          || key == "show_hidden"
          || key == "folders_first"
          || key == "hidden_last"
          || key == "sort_descending"
          || key == "show_menu_bar"
          || key == "always_show_tab_bar"
          || key == "open_tabs_in_background"
          || key == "readline_shortcuts"
          || key == "fzf_search"
          || key == "avoid_redundant_extraction_folders") {
        return node.is_boolean();
      }
      if (key == "preview_mode" || key == "thumbnail_policy") {
        const auto value = node.value_exact<std::string_view>();
        return value
            && (key == "preview_mode" ? *value == "full" || *value == "information" || *value == "hidden"
                                      : *value == "never" || *value == "local" || *value == "all");
      }
      if (key == "thumbnail_max_file_mib" || key == "thumbnail_cache_mib") {
        const auto value = node.value_exact<std::int64_t>();
        return value
            && (key == "thumbnail_max_file_mib" ? *value >= 1 && *value <= 64 : *value >= 16 && *value <= 8192);
      }
      if (key == "search_default_mode") {
        constexpr std::array<std::string_view, 7> modes{"contiguous", "fuzzy", "starts_with", "ends_with",
                                                        "exclude",    "exact", "none"};
        const auto value = node.value_exact<std::string_view>();
        return value && std::ranges::contains(modes, *value);
      }
      if (key == "sort_field" || key == "admin_inactivity_timeout_seconds") {
        const auto value = node.value_exact<std::int64_t>();
        return value && (key == "sort_field" ? *value >= 0 && *value <= 5 : *value >= 60 && *value <= 3600);
      }
      if (key == "search_sort_field") {
        constexpr std::array fields{
            FileDialogSortField::Name, FileDialogSortField::Size, FileDialogSortField::Modified,
            FileDialogSortField::Type, FileDialogSortField::Location
        };
        const auto value = node.value_exact<std::int64_t>();
        return value && std::ranges::any_of(fields, [&](auto field) { return *value == static_cast<int>(field); });
      }
      if (key == "view_scale" || key == "search_view_scale") {
        const auto value = node.value<double>();
        return value
            && std::isfinite(*value)
            && *value >= static_cast<double>(0.8)
            && *value <= static_cast<double>(1.8);
      }
      const bool widthsKey = key == "list_column_widths" || key == "search_column_widths";
      if (widthsKey || key == "list_column_layout" || key == "search_column_layout") {
        const auto text = node.value_exact<std::string_view>();
        if (!text)
          return false;
        if (widthsKey)
          return validColumnWidths(*text);
        return key == "search_column_layout" ? validSearchColumnLayout(*text) : validColumnLayout(*text);
      }
      if (key == "place_order" || key == "hidden_mounts") {
        const auto* array = node.as_array();
        return array && (array->empty() || array->is_homogeneous(toml::node_type::string));
      }
      return false;
    }

    std::optional<ConfigKeyPath> invalidSavedPlace(const toml::node& node) {
      const auto* places = node.as_table();
      if (places == nullptr) {
        return ConfigKeyPath{"files", "saved_places"};
      }
      for (const auto& [id, entry] : *places) {
        const ConfigKeyPath record{"files", "saved_places", std::string(id.str())};
        const auto* place = entry.as_table();
        if (place == nullptr || id.str().empty()) {
          return record;
        }
        for (const auto& [key, value] : *place) {
          bool valid = false;
          if (key == "path") {
            const auto path = value.value_exact<std::string>();
            valid = path && fs::path(*path).is_absolute();
          } else if (key == "label") {
            valid = value.is_string();
          } else if (key == "order") {
            valid = value.value_exact<std::int64_t>().value_or(-1) >= 0;
          }
          if (!valid) {
            auto member = record;
            member.emplace_back(key.str());
            return member;
          }
        }
        if (!place->contains("path") || !place->contains("label")) {
          return record;
        }
      }
      return std::nullopt;
    }

    bool validTransparencyMode(toml::node_view<const toml::node> node) {
      const auto name = node.value_exact<std::string_view>();
      for (const auto& option : kPanelTransparencyModes) {
        if (name && *name == option.key) {
          return true;
        }
      }
      return false;
    }
  } // namespace

  std::optional<ConfigurationProblem> validateFilesConfiguration(const SourcedMergeResult& loaded) {
    if (!loaded.config.firstError.empty()) {
      return ConfigurationProblem{loaded.config.firstErrorOrigin, loaded.config.firstError};
    }

    const auto invalid = [&](const ConfigKeyPath& path, std::string message) {
      if (const auto* source = loaded.sources.winner(path)) {
        return ConfigurationProblem{source->origin, std::move(message)};
      }
      return ConfigurationProblem{{}, std::move(message)};
    };

    for (const auto& [section, node] : loaded.config.merged) {
      const auto* table = node.as_table();
      if (table == nullptr || (section != "files" && section != "theme")) {
        return invalid({std::string(section.str())}, std::format("Invalid Files section '{}'", section.str()));
      }
      for (const auto& [key, value] : *table) {
        if (section == "files" && key == "saved_places") {
          if (const auto path = invalidSavedPlace(value)) {
            std::string name;
            for (const auto& segment : *path) {
              name = schema::joinPath(name, segment);
            }
            return invalid(*path, std::format("Invalid Files setting '{}'", name));
          }
          continue;
        }
        const bool valid = section == "files"
            ? validPreference(key.str(), value)
            : key == "transparency_mode" && validTransparencyMode(toml::node_view(&value));
        if (!valid) {
          return invalid(
              {std::string(section.str()), std::string(key.str())},
              std::format("Invalid Files setting '{}.{}'", section.str(), key.str())
          );
        }
      }
    }

    return std::nullopt;
  }

} // namespace nythera::config
