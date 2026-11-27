#include "render/core/freedesktop_thumbnail.h"

#include "render/core/image_encoder.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <glib.h>
#include <zlib.h>

namespace freedesktop_thumbnail {
  namespace {
    constexpr std::array<std::uint8_t, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
    constexpr std::size_t kMaxText = 128 * 1024;
    constexpr std::string_view kNanoseconds = "X-Nythera::MTimeNS";

    std::uint32_t read32(const std::uint8_t* p) {
      return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
    }

    void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
      for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }

    void appendText(std::vector<std::uint8_t>& bytes, std::string_view key, std::string_view value) {
      append32(bytes, static_cast<std::uint32_t>(key.size() + 1 + value.size()));
      const auto start = bytes.size();
      bytes.insert(bytes.end(), {'t', 'E', 'X', 't'});
      bytes.insert(bytes.end(), key.begin(), key.end());
      bytes.push_back(0);
      bytes.insert(bytes.end(), value.begin(), value.end());
      append32(bytes, static_cast<std::uint32_t>(crc32_z(0, bytes.data() + start, bytes.size() - start)));
    }

    std::optional<std::string> textValue(std::string_view type, std::string_view data) {
      bool compressed = type == "zTXt";
      if (type == "iTXt") {
        if (data.size() < 4 || data[0] > 1 || data[1] != 0)
          return std::nullopt;
        compressed = data[0] == 1;
        data.remove_prefix(2);
        for (int i = 0; i < 2; ++i) {
          const auto separator = data.find('\0');
          if (separator == std::string_view::npos)
            return std::nullopt;
          data.remove_prefix(separator + 1);
        }
      } else if (compressed) {
        if (data.empty() || data[0] != 0)
          return std::nullopt;
        data.remove_prefix(1);
      }
      if (!compressed)
        return data.size() <= kMaxText ? std::optional(std::string(data)) : std::nullopt;
      std::string result(kMaxText, '\0');
      uLongf length = result.size();
      if (uncompress(
              reinterpret_cast<Bytef*>(result.data()), &length, reinterpret_cast<const Bytef*>(data.data()), data.size()
          )
          != Z_OK)
        return std::nullopt;
      result.resize(length);
      return result;
    }
  } // namespace

  int sizeClass(int targetPx) {
    const auto bucket = std::ranges::lower_bound(sizes, targetPx);
    return bucket != std::end(sizes) ? *bucket : 0;
  }

  std::string_view directory(int size) {
    switch (size) {
    case 128:
      return "normal";
    case 256:
      return "large";
    case 512:
      return "x-large";
    case 1024:
      return "xx-large";
    default:
      return {};
    }
  }

  std::optional<Identity> identify(const std::string& path, const struct stat& source) {
    const std::filesystem::path filename(path);
    if (!filename.is_absolute())
      return std::nullopt;
    g_autofree gchar* uri = g_filename_to_uri(filename.lexically_normal().c_str(), nullptr, nullptr);
    if (uri == nullptr)
      return std::nullopt;
    g_autofree gchar* hash = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    return Identity{
        uri, std::string(hash) + ".png", std::to_string(source.st_mtim.tv_sec), std::to_string(source.st_size),
        std::to_string(source.st_mtim.tv_nsec)
    };
  }

  bool matches(std::span<const std::uint8_t> png, const Identity& identity, int maxSide) {
    if (png.size() < 33
        || !std::ranges::equal(png.first(8), kSignature)
        || read32(png.data() + 8) != 13
        || std::string_view(reinterpret_cast<const char*>(png.data() + 12), 4) != "IHDR")
      return false;
    for (const auto offset : {16, 20}) {
      const auto dimension = read32(png.data() + offset);
      if (dimension == 0 || dimension > static_cast<std::uint32_t>(maxSide))
        return false;
    }

    bool foundUri = false;
    bool foundMtime = false;
    bool foundSize = false;
    bool foundNs = false;
    for (std::size_t offset = 8; png.size() - offset >= 12;) {
      const auto size = read32(png.data() + offset);
      if (size > png.size() - offset - 12
          || crc32(0, png.data() + offset + 4, size + 4) != read32(png.data() + offset + 8 + size))
        return false;
      const std::string_view type(reinterpret_cast<const char*>(png.data() + offset + 4), 4);
      if (type == "IEND")
        return size == 0 && offset + 12 == png.size() && foundUri && foundMtime;
      if (type == "tEXt" || type == "zTXt" || type == "iTXt") {
        std::string_view data(reinterpret_cast<const char*>(png.data() + offset + 8), size);
        const auto separator = data.find('\0');
        const auto key = data.substr(0, separator);
        const std::string* expected = nullptr;
        bool* found = nullptr;
        if (key == "Thumb::URI") {
          expected = &identity.uri;
          found = &foundUri;
        } else if (key == "Thumb::MTime") {
          expected = &identity.mtime;
          found = &foundMtime;
        } else if (key == "Thumb::Size") {
          expected = &identity.size;
          found = &foundSize;
        } else if (key == kNanoseconds) {
          expected = &identity.mtimeNanoseconds;
          found = &foundNs;
        }
        if (found != nullptr) {
          if (*found || separator == std::string_view::npos || textValue(type, data.substr(separator + 1)) != *expected)
            return false;
          *found = true;
        }
      }
      offset += size + 12;
    }
    return false;
  }

  bool writtenBy(std::span<const std::uint8_t> png, std::string_view software) {
    if (png.size() < 33 || !std::ranges::equal(png.first(8), kSignature))
      return false;
    constexpr std::string_view prefix{"Software\0", 9};
    // The encoder writes this marker before the image data.
    for (std::size_t offset = 8; png.size() - offset >= 12;) {
      const auto size = read32(png.data() + offset);
      if (size > png.size() - offset - 12)
        return false;
      const std::string_view type(reinterpret_cast<const char*>(png.data() + offset + 4), 4);
      if (type == "IDAT" || type == "IEND")
        return false;
      if (type == "tEXt") {
        const std::string_view payload(reinterpret_cast<const char*>(png.data() + offset + 8), size);
        if (payload.starts_with(prefix) && payload.substr(prefix.size()) == software)
          return crc32(0, png.data() + offset + 4, size + 4) == read32(png.data() + offset + 8 + size);
      }
      offset += size + 12;
    }
    return false;
  }

  std::vector<std::uint8_t>
  encode(const std::uint8_t* rgba, int width, int height, const Identity& identity, std::string_view software) {
    auto png = encodePng(rgba, width, height);
    if (png.size() < 33 || identity.uri.size() > kMaxText)
      return {};
    std::vector<std::uint8_t> metadata;
    appendText(metadata, "Thumb::URI", identity.uri);
    appendText(metadata, "Thumb::MTime", identity.mtime);
    appendText(metadata, "Thumb::Size", identity.size);
    appendText(metadata, kNanoseconds, identity.mtimeNanoseconds);
    appendText(metadata, "Software", software);
    png.insert(png.begin() + 33, metadata.begin(), metadata.end());
    return png;
  }
} // namespace freedesktop_thumbnail
