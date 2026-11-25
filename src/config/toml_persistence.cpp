#include "config/toml_persistence.h"

#include "config/atomic_file.h"

#include <algorithm>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <utility>

// The system CMake target is header-only. Give toml11 its own namespace even
// at link time because both parsers otherwise declare types under ::toml.
#define toml nythera_toml11
#include <toml.hpp>
#undef toml

#ifdef TOML11_COMPILE_SOURCES
#error "NytheraLib requires the header-only toml11 package"
#endif

namespace nythera::config {

  namespace {

    namespace fs = std::filesystem;
    namespace editor = nythera_toml11;
    using Document = editor::ordered_value;
    constexpr editor::spec kTomlVersion(editor::make_semver(1, 0, 0));

    struct Snapshot {
      std::optional<fs::perms> permissions;
      std::string content;

      bool operator==(const Snapshot&) const = default;
    };

    struct Target {
      Snapshot before;
      std::vector<fs::path> aliases;
      std::vector<TomlEdit> edits;
    };

    fs::path writeTarget(const fs::path& path) {
      const auto target = resolveAtomicWriteTarget(path);
      if (!target) {
        throw std::runtime_error("cannot resolve write target");
      }
      const auto resolved = fs::weakly_canonical(fs::absolute(target->path));
      if (fs::is_symlink(fs::symlink_status(resolved))) {
        throw std::runtime_error("cannot resolve symlink chain");
      }
      return resolved;
    }

    Snapshot readSnapshot(const fs::path& path) {
      const auto status = fs::symlink_status(path);
      if (!fs::exists(status)) {
        return {};
      }
      if (!fs::is_regular_file(status)) {
        throw std::runtime_error("write target is not a regular file");
      }
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        throw std::runtime_error("cannot read write target");
      }
      return {status.permissions(), std::string(std::istreambuf_iterator<char>(input), {})};
    }

    void preserveScalarStyle(Document& replacement, const Document& old) {
      if (replacement.is_integer() && old.is_integer() && replacement.as_integer() >= 0) {
        replacement.as_integer_fmt() = old.as_integer_fmt();
      }
      if (replacement.is_string()
          && old.is_string()
          && old.as_string_fmt().fmt == editor::string_format::literal
          && std::ranges::all_of(replacement.as_string(), [](unsigned char c) {
               return c == '\t' || (c >= ' ' && c != '\'' && c != 0x7f);
             })) {
        replacement.as_string_fmt() = old.as_string_fmt();
      }
    }

    bool applyEdit(Document& document, const TomlEdit& edit) {
      if (edit.path.empty()) {
        throw std::runtime_error("cannot edit an empty key path");
      }
      Document* parent = &document;
      for (const std::string& key : std::span(edit.path).first(edit.path.size() - 1)) {
        auto& table = parent->as_table();
        auto child = table.find(key);
        if (child == table.end()) {
          if (!edit.value) {
            return false;
          }
          const auto fmt = parent->as_table_fmt().fmt;
          Document created(Document::table_type{});
          if (fmt == editor::table_format::oneline || fmt == editor::table_format::dotted) {
            created.as_table_fmt().fmt = fmt;
          }
          table[key] = std::move(created);
          child = table.find(key);
        }
        if (!child->second.is_table()) {
          if (!edit.value) {
            return false;
          }
          throw std::runtime_error(std::format("key '{}' is not a table", key));
        }
        parent = &child->second;
      }

      auto& table = parent->as_table();
      const auto& key = edit.path.back();
      const auto old = table.find(key);
      if (!edit.value) {
        if (old == table.end()) {
          return false;
        }
        table.erase(old);
        return true;
      }

      auto wrapped = editor::parse_str<editor::ordered_type_config>("value = " + *edit.value, kTomlVersion);
      if (wrapped.as_table().size() != 1 || !wrapped.contains("value")) {
        throw std::runtime_error("expected one TOML value");
      }
      Document replacement = std::move(wrapped.at("value"));
      if (old != table.end()) {
        replacement.comments() = old->second.comments();
        if (replacement == old->second) {
          return false;
        }
        preserveScalarStyle(replacement, old->second);
      }
      table[key] = std::move(replacement);
      return true;
    }

    std::string stage(const fs::path& path, const Target& target) {
      std::istringstream input(target.before.content);
      auto document = editor::parse<editor::ordered_type_config>(input, path.string(), kTomlVersion);
      bool changed = false;
      for (const auto& edit : target.edits) {
        changed = applyEdit(document, edit) || changed;
      }
      if (!changed) {
        return target.before.content;
      }
      auto content = editor::format(document, kTomlVersion);
      // Check with the same parser used by the configuration loader.
      static_cast<void>(toml::parse(content, path.string()));
      return content;
    }

    std::string describe(const fs::path& path, std::string_view error) {
      return std::format("{}: {}", path.string(), error);
    }

    bool restore(const StagedTomlFile& file, const Snapshot& before) {
      try {
        const auto current = readSnapshot(file.path);
        if (!current.permissions || current.content != file.content) {
          return false;
        }
        if (before.permissions) {
          return writeTextFileAtomic(file.path, before.content, before.permissions);
        }
        return fs::remove(file.path);
      } catch (const std::exception&) {
        return false;
      }
    }

  } // namespace

  [[gnu::visibility("default")]] TomlPersistenceResult
  persistTomlEdits(std::span<const TomlFileEdits> files, const TomlWriteValidator& validate) {
    static std::mutex writeMutex;
    const std::lock_guard lock(writeMutex);
    std::map<fs::path, Target> targets;
    std::vector<fs::path> order;
    std::vector<StagedTomlFile> staged;
    fs::path current;
    try {
      for (const auto& file : files) {
        if (file.edits.empty()) {
          continue;
        }
        current = file.path;
        const auto resolved = writeTarget(file.path);
        auto [target, inserted] = targets.try_emplace(resolved);
        if (inserted) {
          order.push_back(resolved);
          target->second.before = readSnapshot(resolved);
        }
        target->second.aliases.push_back(file.path);
        target->second.edits.append_range(file.edits);
      }
      for (const auto& path : order) {
        current = path;
        const auto& target = targets.at(path);
        auto content = stage(path, target);
        if (content != target.before.content) {
          if (target.before.permissions && ::faccessat(AT_FDCWD, path.c_str(), W_OK, AT_EACCESS) != 0) {
            throw std::runtime_error("write target is not writable");
          }
          staged.push_back({path, std::move(content), target.aliases});
        }
      }
      if (validate && !staged.empty()) {
        current.clear();
        if (auto error = validate(staged)) {
          return {.error = error->empty() ? "validation rejected the staged documents" : std::move(*error)};
        }
      }
    } catch (const std::exception& error) {
      return {.error = current.empty() ? error.what() : describe(current, error.what())};
    }

    std::size_t written = 0;
    TomlPersistenceResult result;
    try {
      for (const auto& file : staged) {
        current = file.path;
        const auto& target = targets.at(current);
        for (const auto& alias : target.aliases) {
          if (writeTarget(alias) != current) {
            throw std::runtime_error("write target moved since staging");
          }
        }
        if (readSnapshot(current) != target.before) {
          throw std::runtime_error("write target changed since staging");
        }
        if (!writeTextFileAtomic(current, file.content, target.before.permissions)) {
          throw std::runtime_error("atomic replacement failed");
        }
        ++written;
      }
    } catch (const std::exception& error) {
      result.error = describe(current, error.what());
      while (written > 0) {
        const auto& file = staged[--written];
        if (!restore(file, targets.at(file.path).before)) {
          result.rollbackFailures.push_back(file.path);
        }
      }
    }
    return result;
  }

} // namespace nythera::config
