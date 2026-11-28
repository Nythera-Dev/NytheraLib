#include "render/core/svg_rasterizer.h"

#include <algorithm>
#include <array>
#include <cairo.h>
#include <cmath>
#include <gio/gio.h>
#include <limits>
#include <utility>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#include <librsvg/rsvg.h>
#pragma GCC diagnostic pop

namespace svg_rasterizer {

  namespace {

    [[nodiscard]] bool isGzipHeader(const std::vector<std::uint8_t>& data) noexcept {
      return data.size() >= 2 && data[0] == 0x1FU && data[1] == 0x8BU;
    }

    [[nodiscard]] bool asciiWhitespace(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

    [[nodiscard]] char asciiLower(char ch) noexcept {
      return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    std::expected<std::vector<std::uint8_t>, std::string>
    decompressGzipBounded(const std::uint8_t* data, std::size_t size, std::size_t maxOutputBytes) {
      if (data == nullptr || size == 0) {
        return std::unexpected("empty compressed SVGZ input");
      }
      GInputStream* memStream = g_memory_input_stream_new_from_data(data, static_cast<gssize>(size), nullptr);
      GZlibDecompressor* decompressor = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      GInputStream* convStream = g_converter_input_stream_new(memStream, G_CONVERTER(decompressor));
      g_object_unref(decompressor);
      g_object_unref(memStream);

      std::vector<std::uint8_t> out;
      std::array<std::uint8_t, 16384> chunk{};
      GError* err = nullptr;
      while (true) {
        const gssize n = g_input_stream_read(convStream, chunk.data(), chunk.size(), nullptr, &err);
        if (n < 0) {
          std::string msg = err != nullptr ? err->message : "gzip decompression failed";
          if (err != nullptr) {
            g_error_free(err);
          }
          g_object_unref(convStream);
          return std::unexpected(std::move(msg));
        }
        if (n == 0) {
          break;
        }
        if (out.size() + static_cast<std::size_t>(n) > maxOutputBytes) {
          g_object_unref(convStream);
          return std::unexpected("decompressed SVGZ exceeds size limit");
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + n);
      }
      g_object_unref(convStream);
      return out;
    }

    void argb32ToRgba(const unsigned char* src, int srcStride, std::uint8_t* dst, int width, int height) {
      for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(src + (y * srcStride));
        std::uint8_t* outRow = dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
        for (int x = 0; x < width; ++x) {
          const std::uint32_t pixel = row[x];
          const auto a = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
          auto r = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
          auto g = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
          auto b = static_cast<std::uint8_t>(pixel & 0xFFU);
          if (a != 0 && a != 255) {
            r = static_cast<std::uint8_t>(std::min(255, ((r * 255) + (a / 2)) / a));
            g = static_cast<std::uint8_t>(std::min(255, ((g * 255) + (a / 2)) / a));
            b = static_cast<std::uint8_t>(std::min(255, ((b * 255) + (a / 2)) / a));
          }
          outRow[(x * 4) + 0] = r;
          outRow[(x * 4) + 1] = g;
          outRow[(x * 4) + 2] = b;
          outRow[(x * 4) + 3] = a;
        }
      }
    }

  } // namespace

  [[nodiscard]] bool looksLikeSvg(const std::vector<std::uint8_t>& data) noexcept {
    std::size_t pos = (data.size() >= 3 && data[0] == 0xEFU && data[1] == 0xBBU && data[2] == 0xBFU) ? 3U : 0U;
    while (pos < data.size() && asciiWhitespace(static_cast<char>(data[pos]))) {
      ++pos;
    }
    if (pos >= data.size() || data[pos] != '<') {
      return false;
    }
    const std::size_t scanEnd = std::min(data.size(), pos + 4096U);
    for (; pos + 4 <= scanEnd; ++pos) {
      if (data[pos] == '<'
          && asciiLower(static_cast<char>(data[pos + 1])) == 's'
          && asciiLower(static_cast<char>(data[pos + 2])) == 'v'
          && asciiLower(static_cast<char>(data[pos + 3])) == 'g') {
        if (pos + 4 == data.size()) {
          return true;
        }
        const char next = static_cast<char>(data[pos + 4]);
        if (asciiWhitespace(next) || next == '>' || next == '/' || next == ':') {
          return true;
        }
      }
    }
    return false;
  }

  std::expected<void, std::string> inflateIfCompressed(std::vector<std::uint8_t>& bytes) {
    if (!isGzipHeader(bytes)) {
      return {};
    }
    if (bytes.size() > kMaxSourceBytes) {
      return std::unexpected("SVGZ source exceeds size limit");
    }

    auto decompressed = decompressGzipBounded(bytes.data(), bytes.size(), kMaxSourceBytes);
    if (!decompressed) {
      return std::unexpected(std::move(decompressed.error()));
    }
    if (isGzipHeader(*decompressed)) {
      return std::unexpected("nested gzip streams are unsupported in SVGZ");
    }
    bytes = std::move(*decompressed);
    return {};
  }

  std::expected<DecodedRasterImage, std::string>
  rasterize(const std::vector<std::uint8_t>& bytes, int targetSize, Sizing sizing) {
    if (bytes.empty()) {
      return std::unexpected("empty SVG input");
    }
    if (bytes.size() > kMaxSourceBytes || isGzipHeader(bytes)) {
      return std::unexpected("SVG rasterization requires uncompressed input within the source size limit");
    }

    // Load from memory with base_file == nullptr and RSVG_HANDLE_FLAGS_NONE:
    // blocks external file:// and relative URIs, prevents network loads, and keeps
    // libxml2 / librsvg element, nesting, and recursion safety limits active.
    GInputStream* memStream =
        g_memory_input_stream_new_from_data(bytes.data(), static_cast<gssize>(bytes.size()), nullptr);
    GError* gerror = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_stream_sync(memStream, nullptr, RSVG_HANDLE_FLAGS_NONE, nullptr, &gerror);
    g_object_unref(memStream);
    if (handle == nullptr) {
      std::string error = std::string("failed to parse SVG: ") + (gerror != nullptr ? gerror->message : "unknown");
      if (gerror != nullptr) {
        g_error_free(gerror);
      }
      return std::unexpected(std::move(error));
    }

    gdouble intrinsicW = 0.0;
    gdouble intrinsicH = 0.0;
    const gboolean hasIntrinsic = rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsicW, &intrinsicH);
    if (hasIntrinsic == FALSE
        || !std::isfinite(intrinsicW)
        || !std::isfinite(intrinsicH)
        || intrinsicW <= 0.0
        || intrinsicH <= 0.0) {
      gboolean outHasW = FALSE;
      RsvgLength outW{};
      gboolean outHasH = FALSE;
      RsvgLength outH{};
      gboolean outHasViewbox = FALSE;
      RsvgRectangle outViewbox{};
      rsvg_handle_get_intrinsic_dimensions(handle, &outHasW, &outW, &outHasH, &outH, &outHasViewbox, &outViewbox);
      if (outHasViewbox == TRUE) {
        if (!std::isfinite(outViewbox.width)
            || !std::isfinite(outViewbox.height)
            || outViewbox.width <= 0.0
            || outViewbox.height <= 0.0) {
          g_object_unref(handle);
          return std::unexpected("invalid SVG viewBox dimensions");
        }
        intrinsicW = outViewbox.width;
        intrinsicH = outViewbox.height;
      } else if (
          sizing == Sizing::Thumbnail
          && outHasW == TRUE
          && outHasH == TRUE
          && outW.unit != RSVG_UNIT_PERCENT
          && outH.unit != RSVG_UNIT_PERCENT
      ) {
        if (!std::isfinite(outW.length) || !std::isfinite(outH.length) || outW.length <= 0.0 || outH.length <= 0.0) {
          g_object_unref(handle);
          return std::unexpected("invalid SVG intrinsic dimensions");
        }
        intrinsicW = outW.length;
        intrinsicH = outH.length;
      } else {
        intrinsicW = sizing == Sizing::Thumbnail ? static_cast<double>(targetSize) : 512.0;
        intrinsicH = intrinsicW;
      }
    }

    const double maxSide = std::max(intrinsicW, intrinsicH);
    if (!std::isfinite(maxSide) || maxSide <= 0.0) {
      g_object_unref(handle);
      return std::unexpected("invalid SVG aspect ratio");
    }
    double roundedWidth = std::round(intrinsicW);
    double roundedHeight = std::round(intrinsicH);
    if (targetSize > 0) {
      const double target = static_cast<double>(targetSize);
      const double scale = target / maxSide;
      // Normalise first when very small intrinsic dimensions overflow the scale.
      const auto fit = [&](double side) {
        const double fitted = std::isfinite(scale) ? side * scale : (side / maxSide) * target;
        return std::clamp(std::round(fitted), 1.0, target);
      };
      roundedWidth = fit(intrinsicW);
      roundedHeight = fit(intrinsicH);
    }
    if (!std::isfinite(roundedWidth)
        || !std::isfinite(roundedHeight)
        || roundedWidth < 1.0
        || roundedHeight < 1.0
        || roundedWidth > std::numeric_limits<int>::max()
        || roundedHeight > std::numeric_limits<int>::max()) {
      g_object_unref(handle);
      return std::unexpected("invalid SVG dimensions");
    }
    const int width = static_cast<int>(roundedWidth);
    const int height = static_cast<int>(roundedHeight);

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surface);
      g_object_unref(handle);
      return std::unexpected("failed to create cairo surface for SVG");
    }

    cairo_t* cr = cairo_create(surface);
    const RsvgRectangle viewport{
        .x = 0.0,
        .y = 0.0,
        .width = static_cast<double>(width),
        .height = static_cast<double>(height),
    };
    GError* renderError = nullptr;
    if (rsvg_handle_render_document(handle, cr, &viewport, &renderError) == FALSE) {
      std::string error =
          std::string("failed to render SVG: ") + (renderError != nullptr ? renderError->message : "unknown");
      if (renderError != nullptr) {
        g_error_free(renderError);
      }
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
      g_object_unref(handle);
      return std::unexpected(std::move(error));
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    DecodedRasterImage decoded{
        .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U),
        .width = width,
        .height = height,
    };
    argb32ToRgba(
        cairo_image_surface_get_data(surface), cairo_image_surface_get_stride(surface), decoded.pixels.data(), width,
        height
    );

    cairo_surface_destroy(surface);
    g_object_unref(handle);
    return decoded;
  }

} // namespace svg_rasterizer
