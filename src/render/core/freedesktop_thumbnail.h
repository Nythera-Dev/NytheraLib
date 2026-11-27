#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace freedesktop_thumbnail {
  struct Identity {
    std::string uri;
    std::string leaf;
    std::string mtime;
    std::string size;
    std::string mtimeNanoseconds;
  };

  inline constexpr int sizes[] = {128, 256, 512, 1024};
  [[nodiscard]] int sizeClass(int targetPx);
  [[nodiscard]] std::string_view directory(int size);
  [[nodiscard]] std::optional<Identity> identify(const std::string& path, const struct stat& source);
  [[nodiscard]] bool matches(std::span<const std::uint8_t> png, const Identity& identity, int maxSide);
  [[nodiscard]] bool writtenBy(std::span<const std::uint8_t> png, std::string_view software);
  [[nodiscard]] std::vector<std::uint8_t>
  encode(const std::uint8_t* rgba, int width, int height, const Identity& identity, std::string_view software);
} // namespace freedesktop_thumbnail
