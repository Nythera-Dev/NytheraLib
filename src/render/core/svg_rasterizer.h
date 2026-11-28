#pragma once

#include "render/core/image_decoder.h"

namespace svg_rasterizer {

  inline constexpr std::size_t kMaxSourceBytes = 16ULL * 1024ULL * 1024ULL;

  enum class Sizing {
    Thumbnail, // Fit to the requested size, using it as the fallback extent.
    Image,     // A nonpositive target keeps native size, with a 512 px fallback.
  };

  [[nodiscard, gnu::visibility("hidden")]] bool looksLikeSvg(const std::vector<std::uint8_t>& bytes) noexcept;

  // SVGZ input and output share the source limit. Plain input passes through unchanged.
  [[nodiscard, gnu::visibility("hidden")]] std::expected<void, std::string>
  inflateIfCompressed(std::vector<std::uint8_t>& bytes);

  [[nodiscard, gnu::visibility("hidden")]] std::expected<DecodedRasterImage, std::string>
  rasterize(const std::vector<std::uint8_t>& bytes, int targetSize, Sizing sizing);

} // namespace svg_rasterizer
