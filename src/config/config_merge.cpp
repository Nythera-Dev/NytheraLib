#include "config/config_merge.h"

#include "config/config_service.h"
#include "config/toml_persistence.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <algorithm>
#include <format>
#include <map>
#include <system_error>
#include <utility>

namespace nythera::config {

  namespace {

    constexpr Logger kLog("config");

    // Sorted *.toml directly in `dir` (non-recursive). Mirrors the root-scan in
    // ConfigService so directory includes load in a stable, predictable order.
    std::vector<std::filesystem::path>
    sortedTomlInDir(const std::filesystem::path& dir, std::vector<std::filesystem::path>* watchPaths) {
      std::vector<std::filesystem::path> files;
      std::error_code ec;
      if (!std::filesystem::is_directory(dir, ec) || ec) {
        return files;
      }
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".toml")
          continue;
        if (entry.is_regular_file()) {
          files.push_back(entry.path());
        } else if (watchPaths && entry.is_symlink()) {
          watchPaths->push_back(entry.path());
        }
      }
      std::ranges::sort(files);
      return files;
    }

    std::filesystem::path canonicalKey(const std::filesystem::path& path) {
      std::error_code ec;
      auto key = std::filesystem::weakly_canonical(path, ec);
      return ec ? path.lexically_normal() : key;
    }

    struct LoadInputs {
      std::span<const StagedTomlFile> staged;
      std::vector<std::filesystem::path>* watchPaths = nullptr;

      void watch(const std::filesystem::path& path) const {
        if (watchPaths) {
          watchPaths->push_back(path);
        }
      }

      const StagedTomlFile* replacement(const std::filesystem::path& path) const {
        if (staged.empty()) {
          return nullptr;
        }
        const auto key = canonicalKey(path);
        for (const auto& file : staged) {
          if (file.path == key
              || std::ranges::any_of(file.aliases, [&](const auto& alias) { return canonicalKey(alias) == key; })) {
            return &file;
          }
        }
        return nullptr;
      }

      std::filesystem::path canonicalFile(const std::filesystem::path& path) const {
        if (const auto* file = replacement(path)) {
          return file->path;
        }
        return canonicalKey(path);
      }

      toml::table parse(const std::filesystem::path& path) const {
        if (const auto* file = replacement(path)) {
          return toml::parse(file->content, path.string());
        }
        return toml::parse_file(path.string());
      }

      std::vector<std::filesystem::path> scan(const std::filesystem::path& dir) const {
        auto documents = sortedTomlInDir(dir, watchPaths);
        if (staged.empty()) {
          return documents;
        }
        const auto canonicalDir = canonicalKey(dir);
        const auto append = [&](const auto& path) {
          if (path.extension() == ".toml" && canonicalKey(path.parent_path()) == canonicalDir) {
            documents.push_back(dir / path.filename());
          }
        };
        for (const auto& file : staged) {
          append(file.path);
          for (const auto& alias : file.aliases) {
            append(alias);
          }
        }
        std::ranges::sort(documents);
        documents.erase(std::unique(documents.begin(), documents.end()), documents.end());
        return documents;
      }

      void validateInclude(const std::filesystem::path& path, const toml::table& table, MergeResult& out) const {
        if (!watchPaths || !out.firstError.empty()) {
          return;
        }
        const auto* node = table.get("include");
        if (!node) {
          return;
        }
        if (const auto* include = node->as_table()) {
          const auto* autoload = include->get("autoload");
          const auto* files = include->get("files");
          if ((!autoload || autoload->is_boolean())
              && (!files
                  || (files->is_array()
                      && (files->as_array()->empty() || files->as_array()->is_homogeneous(toml::node_type::string))))) {
            return;
          }
        }
        const auto& pos = node->source().begin;
        out.firstErrorOrigin = {path.string(), pos.line, pos.column};
        out.firstError = "expected [include] with a Boolean autoload and an array of file names";
      }
    };

    struct IncludeEntry {
      std::string value;
      schema::SourceOrigin origin;
    };

    struct IncludeDirective {
      std::vector<IncludeEntry> files;
      bool autoload = true;
      bool hasAutoload = false;
    };

    IncludeDirective readInclude(const std::filesystem::path& path, const toml::table& tbl) {
      IncludeDirective directive;
      const auto* inc = tbl["include"].as_table();
      if (inc == nullptr) {
        return directive;
      }
      if (auto v = (*inc)["autoload"].value<bool>()) {
        directive.autoload = *v;
        directive.hasAutoload = true;
      }
      if (const auto* arr = (*inc)["files"].as_array()) {
        for (const auto& node : *arr) {
          if (auto s = node.value<std::string>()) {
            const auto& src = node.source();
            directive.files.push_back(
                IncludeEntry{std::move(*s), schema::SourceOrigin{path.string(), src.begin.line, src.begin.column}}
            );
          }
        }
      }
      return directive;
    }

    enum class Visit { Expanding, Expanded };
    using IncludeVisits = std::map<std::filesystem::path, Visit>;

    // Forward declaration: parse `path`, then expand it (mutual recursion with the
    // include loop below).
    toml::table loadAndExpand(
        const std::filesystem::path& path, IncludeVisits& visited, MergeResult& out, ConfigSourceMap* sources,
        const LoadInputs& inputs
    );

    void mergeIncluded(
        const std::filesystem::path& path, IncludeVisits& visited, MergeResult& out, toml::table& base,
        ConfigSourceMap* sources, const LoadInputs& inputs
    ) {
      ConfigSourceMap included;
      ConfigService::deepMerge(base, loadAndExpand(path, visited, out, sources ? &included : nullptr, inputs));
      if (sources) {
        sources->overlay(included);
      }
    }

    // Expands an already-parsed file: merges its includes first, then overlays the
    // file's own body (minus [include]) so the host file wins. Returns the merged
    // subtree contributed by this file.
    toml::table expandFile(
        const std::filesystem::path& path, const toml::table& parsed, IncludeVisits& visited, MergeResult& out,
        ConfigSourceMap* sources, const LoadInputs& inputs
    ) {
      const auto key = inputs.canonicalFile(path);
      const auto [visit, firstVisit] = visited.try_emplace(key, Visit::Expanding);
      if (!firstVisit) {
        // A previously expanded file can also be reached by autoload or another include.
        if (visit->second == Visit::Expanding) {
          kLog.warn("config include cycle skipped: {}", key.string());
        }
        return toml::table{};
      }
      out.loadedFiles.push_back(key);

      const std::string includingDir = path.parent_path().string();
      inputs.validateInclude(path, parsed, out);
      const IncludeDirective directive = readInclude(path, parsed);

      toml::table base;
      for (const auto& entry : directive.files) {
        const std::string expanded = FileUtils::expandEnvVars(entry.value);
        const std::filesystem::path target = FileUtils::resolvePath(expanded, includingDir);

        inputs.watch(target);
        std::error_code ec;
        if (std::filesystem::is_directory(target, ec) && !ec) {
          out.includeDirs.push_back(canonicalKey(target));
          for (const auto& child : inputs.scan(target)) {
            mergeIncluded(child, visited, out, base, sources, inputs);
          }
        } else if ((std::filesystem::is_regular_file(target, ec) && !ec) || inputs.replacement(target)) {
          mergeIncluded(target, visited, out, base, sources, inputs);
        } else {
          if (out.firstError.empty()) {
            out.firstError = std::format("include not found: {}", entry.value);
            out.firstErrorOrigin = entry.origin;
          }
          kLog.warn("config include not found: {} (from {})", target.string(), path.string());
        }
      }

      // Recorded after the includes so the host file's keys win here too.
      out.origins.record(path, parsed);
      if (sources) {
        sources->record(path, parsed);
      }

      // Host wins: the file's own body overlays the includes it pulled in.
      toml::table body = parsed;
      body.erase("include");
      ConfigService::deepMerge(base, body);
      visit->second = Visit::Expanded;
      return base;
    }

    toml::table loadAndExpand(
        const std::filesystem::path& path, IncludeVisits& visited, MergeResult& out, ConfigSourceMap* sources,
        const LoadInputs& inputs
    ) {
      inputs.watch(path);
      toml::table parsed;
      try {
        parsed = inputs.parse(path);
      } catch (const toml::parse_error& e) {
        if (out.firstError.empty()) {
          out.firstError = std::string(e.description());
          out.firstErrorOrigin = parseErrorOrigin(e, path);
        }
        kLog.warn("parse error in {}: {}", path.string(), e.description());
        return toml::table{};
      }
      return expandFile(path, parsed, visited, out, sources, inputs);
    }

    void mergeConfig(
        std::string_view configDir, std::span<const std::filesystem::path> baseFiles, MergeResult& out,
        ConfigSourceMap* sources, const LoadInputs& inputs
    ) {
      if (configDir.empty() && baseFiles.empty()) {
        return;
      }

      IncludeVisits visited;
      for (const auto& baseFile : baseFiles) {
        if (baseFile.empty()) {
          continue;
        }
        inputs.watch(baseFile);
        std::error_code ec;
        if ((std::filesystem::is_regular_file(baseFile, ec) && !ec) || inputs.replacement(baseFile) != nullptr) {
          ConfigSourceMap baseSources;
          ConfigService::deepMerge(
              out.merged, loadAndExpand(baseFile, visited, out, sources ? &baseSources : nullptr, inputs)
          );
          if (sources) {
            sources->overlay(baseSources);
          }
        }
      }

      if (configDir.empty()) {
        return;
      }

      // Phase A — parse every root file, noting any [include].autoload = false.
      struct Root {
        std::filesystem::path path;
        toml::table table;
        bool optOut = false;
      };
      std::vector<Root> roots;
      bool anyOptOut = false;
      for (const auto& path : inputs.scan(std::filesystem::path(configDir))) {
        inputs.watch(path);
        toml::table tbl;
        try {
          tbl = inputs.parse(path);
        } catch (const toml::parse_error& e) {
          if (out.firstError.empty()) {
            out.firstError = std::string(e.description());
            out.firstErrorOrigin = parseErrorOrigin(e, path);
          }
          kLog.warn("parse error in {}: {}", path.string(), e.description());
          continue;
        }
        const IncludeDirective directive = readInclude(path, tbl);
        const bool optOut = directive.hasAutoload && !directive.autoload;
        anyOptOut = anyOptOut || optOut;
        roots.push_back(Root{.path = path, .table = std::move(tbl), .optOut = optOut});
      }

      // Phase B — merge the load set. When any root opts out, only the opting-out
      // roots (and their recursive includes) are loaded; other roots are skipped.
      for (const auto& root : roots) {
        if (anyOptOut && !root.optOut) {
          continue;
        }
        ConfigSourceMap rootSources;
        ConfigService::deepMerge(
            out.merged, expandFile(root.path, root.table, visited, out, sources ? &rootSources : nullptr, inputs)
        );
        if (sources) {
          sources->overlay(rootSources);
        }
      }
    }

  } // namespace

  MergeResult mergeConfigWithIncludes(std::string_view configDir) { return mergeConfigWithIncludes(configDir, {}); }

  MergeResult mergeConfigWithIncludes(std::string_view configDir, std::span<const std::filesystem::path> baseFiles) {
    MergeResult result;
    mergeConfig(configDir, baseFiles, result, nullptr, {});
    return result;
  }

  SourcedMergeResult mergeConfigWithSources(std::string_view configDir) {
    return mergeConfigWithSources(configDir, {}, {});
  }

  SourcedMergeResult mergeConfigWithSources(std::string_view configDir, std::span<const StagedTomlFile> staged) {
    return mergeConfigWithSources(configDir, staged, {});
  }

  SourcedMergeResult mergeConfigWithSources(
      std::string_view configDir, std::span<const StagedTomlFile> staged,
      std::span<const std::filesystem::path> baseFiles
  ) {
    SourcedMergeResult result;
    mergeConfig(configDir, baseFiles, result.config, &result.sources, {staged, &result.watchPaths});
    return result;
  }

} // namespace nythera::config
