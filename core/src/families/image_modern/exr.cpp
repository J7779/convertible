// Convertible core - image_modern family: OpenEXR (.exr).
//
// Reader: single-part and multi-part files, scan-line and tiled parts
// (mipmap/ripmap: the full-resolution level), HALF/FLOAT/UINT channels,
// every classic compression (none, RLE, ZIPS, ZIP, PIZ, PXR24, B44, B44A,
// DWAA, DWAB). The RGBA layer (or Y / YA / Y-RY-BY luminance-chroma) is
// converted to straight-alpha linear Rec.709 floats in `ir::Image::hdr`;
// the data window is placed inside the display window, which sets the
// picture size. Other parts of the same size become further frames. The
// 8-bit frame is tone mapped (options `tone_map`, `exposure`).
//
// Writer: one scan-line part, HALF (default) or FLOAT samples, compression
// none, RLE, ZIPS, ZIP (default) or PIZ, R/G/B or Y plus A when needed, and
// only the eight required header attributes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/image_modern/exr.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/exr_internal.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern {

namespace {

using exr::ChunkChannel;
using exr::ChunkLayout;
using exr::Compression;
using exr::PixelType;

constexpr std::uint32_t kMagic = 20000630;
constexpr std::uint32_t kTiledFlag = 0x200;
constexpr std::uint32_t kLongNamesFlag = 0x400;
constexpr std::uint32_t kDeepFlag = 0x800;
constexpr std::uint32_t kMultipartFlag = 0x1000;

// --------------------------------------------------------------------------- header

struct Box {
  std::int32_t xmin = 0;
  std::int32_t ymin = 0;
  std::int32_t xmax = -1;
  std::int32_t ymax = -1;
  [[nodiscard]] std::int64_t width() const noexcept { return std::int64_t{xmax} - xmin + 1; }
  [[nodiscard]] std::int64_t height() const noexcept { return std::int64_t{ymax} - ymin + 1; }
  friend bool operator==(const Box&, const Box&) = default;
};

struct Header {
  std::vector<ChunkChannel> channels;
  bool has_channels = false;
  Compression compression = Compression::None;
  bool has_compression = false;
  Box data_window;
  Box display_window;
  bool has_data_window = false;
  bool has_display_window = false;
  bool has_tiles = false;
  std::uint32_t tile_width = 0;
  std::uint32_t tile_height = 0;
  std::uint8_t level_mode = 0;  // 0 one level, 1 mipmap, 2 ripmap
  std::uint8_t rounding = 0;    // 0 down, 1 up
  std::string type;
  std::string name;
  std::int64_t chunk_count = -1;
  std::optional<std::array<float, 8>> chromaticities;
  // Derived.
  bool tiled = false;
  bool deep = false;
  std::uint64_t computed_chunks = 0;
  std::uint64_t tiles_x = 0;  // level-0 tile grid
  std::uint64_t tiles_y = 0;
};

Result<void> parse_channel_list(ByteView value, Header& header) {
  util::ByteReader r(value);
  while (true) {
    const std::string_view name = r.cstring();
    if (!r.ok()) return fail(Status::Decode, "corrupt OpenEXR channel list");
    if (name.empty()) break;
    ChunkChannel channel;
    channel.name = std::string(name);
    const std::int32_t type = r.i32le();
    channel.p_linear = r.u8() != 0;
    r.skip(3);
    channel.x_sampling = r.i32le();
    channel.y_sampling = r.i32le();
    if (!r.ok()) return fail(Status::Decode, "corrupt OpenEXR channel list");
    if (type < 0 || type > 2) return fail(Status::Decode, "unknown OpenEXR pixel type {}", type);
    if (channel.x_sampling < 1 || channel.y_sampling < 1) return fail(Status::Decode, "invalid OpenEXR channel sampling");
    channel.type = static_cast<PixelType>(type);
    header.channels.push_back(std::move(channel));
  }
  if (header.channels.empty()) return fail(Status::Decode, "the OpenEXR part has no channels");
  // The format requires the list sorted by name; the chunk layout follows it.
  std::stable_sort(header.channels.begin(), header.channels.end(),
                   [](const ChunkChannel& a, const ChunkChannel& b) { return a.name < b.name; });
  header.has_channels = true;
  return {};
}

Box read_box(ByteView value) {
  util::ByteReader r(value);
  Box box;
  box.xmin = r.i32le();
  box.ymin = r.i32le();
  box.xmax = r.i32le();
  box.ymax = r.i32le();
  return box;
}

/// Attributes of one header, up to and including its terminating NUL.
Result<Header> parse_header(util::ByteReader& r) {
  Header header;
  for (int count = 0;; ++count) {
    if (count > 10000) return fail(Status::Decode, "too many OpenEXR header attributes");
    const std::string_view name = r.cstring();
    if (!r.ok()) return fail(Status::Decode, "the OpenEXR header is truncated");
    if (name.empty()) break;
    const std::string_view type = r.cstring();
    const std::int32_t size = r.i32le();
    if (!r.ok() || size < 0 || static_cast<std::size_t>(size) > r.remaining()) {
      return fail(Status::Decode, "the OpenEXR header is truncated");
    }
    const ByteView value = r.bytes(static_cast<std::size_t>(size));
    if (name == "channels" && type == "chlist") {
      CVT_TRY(parse_channel_list(value, header));
    } else if (name == "compression" && type == "compression" && size >= 1) {
      const auto code = std::to_integer<std::uint8_t>(value[0]);
      if (code > 9) {
        const char* what = code == 10 || code == 11 ? "HTJ2K" : code == 12 ? "lossy JPEG 2000" : code == 13 ? "Zstandard" : "unknown";
        return fail(Status::Decode, "the OpenEXR file uses {} compression (method {}), which is not supported", what, code);
      }
      header.compression = static_cast<Compression>(code);
      header.has_compression = true;
    } else if (name == "dataWindow" && type == "box2i" && size >= 16) {
      header.data_window = read_box(value);
      header.has_data_window = true;
    } else if (name == "displayWindow" && type == "box2i" && size >= 16) {
      header.display_window = read_box(value);
      header.has_display_window = true;
    } else if (name == "tiles" && type == "tiledesc" && size >= 9) {
      util::ByteReader t(value);
      header.tile_width = t.u32le();
      header.tile_height = t.u32le();
      const std::uint8_t mode = t.u8();
      header.level_mode = static_cast<std::uint8_t>(mode & 0x0F);
      header.rounding = static_cast<std::uint8_t>(mode >> 4);
      header.has_tiles = true;
    } else if (name == "type" && type == "string") {
      header.type = std::string(as_chars(value));
    } else if (name == "name" && type == "string") {
      header.name = std::string(as_chars(value));
    } else if (name == "chunkCount" && type == "int" && size >= 4) {
      header.chunk_count = static_cast<std::int32_t>(exr::load32(value.data()));
    } else if (name == "chromaticities" && type == "chromaticities" && size >= 32) {
      std::array<float, 8> c{};
      for (std::size_t i = 0; i < 8; ++i) c[i] = std::bit_cast<float>(exr::load32(value.data() + i * 4));
      header.chromaticities = c;
    }
  }
  return header;
}

std::uint32_t round_log2(std::uint64_t x, std::uint8_t rounding) noexcept {
  std::uint32_t y = 0;
  if (rounding == 0) {
    while (x > 1) {
      ++y;
      x >>= 1;
    }
  } else {
    std::uint64_t value = 1;
    while (value < x) {
      ++y;
      value <<= 1;
    }
  }
  return y;
}

std::uint64_t level_size(std::uint64_t base, std::uint32_t level, std::uint8_t rounding) noexcept {
  const std::uint64_t size = rounding == 0 ? base >> level : (base + (std::uint64_t{1} << level) - 1) >> level;
  return std::max<std::uint64_t>(size, 1);
}

/// Check a part's header and derive its chunk structure. `max_chunks`
/// bounds the chunk count by what the file can hold.
Result<void> validate_part(Header& h, bool single_tiled, std::uint64_t max_chunks) {
  h.deep = h.type == "deepscanline" || h.type == "deeptile";
  if (h.deep) return {};
  if (!h.has_channels || !h.has_compression || !h.has_data_window || !h.has_display_window) {
    return fail(Status::Decode, "the OpenEXR header lacks a required attribute");
  }
  h.tiled = h.type.empty() ? single_tiled : h.type == "tiledimage";
  if (!h.type.empty() && h.type != "tiledimage" && h.type != "scanlineimage") {
    return fail(Status::Decode, "unknown OpenEXR part type '{}'", h.type);
  }
  const Box& dw = h.data_window;
  const Box& disp = h.display_window;
  if (dw.width() < 1 || dw.height() < 1 || disp.width() < 1 || disp.height() < 1 || dw.width() > 0x7FFFFFFF ||
      dw.height() > 0x7FFFFFFF || disp.width() > 0x7FFFFFFF || disp.height() > 0x7FFFFFFF) {
    return fail(Status::Decode, "invalid OpenEXR data or display window");
  }
  const auto width = static_cast<std::uint64_t>(dw.width());
  const auto height = static_cast<std::uint64_t>(dw.height());
  if (!h.tiled) {
    for (const ChunkChannel& c : h.channels) {
      if (dw.xmin % c.x_sampling != 0 || dw.ymin % c.y_sampling != 0 ||
          static_cast<std::uint64_t>(dw.width()) % static_cast<std::uint64_t>(c.x_sampling) != 0 ||
          static_cast<std::uint64_t>(dw.height()) % static_cast<std::uint64_t>(c.y_sampling) != 0) {
        return fail(Status::Decode, "OpenEXR channel '{}' has sampling that does not fit the data window", c.name);
      }
    }
    const auto lines = static_cast<std::uint64_t>(exr::lines_per_chunk(h.compression));
    h.computed_chunks = (height + lines - 1) / lines;
  } else {
    if (!h.has_tiles || h.tile_width == 0 || h.tile_height == 0 || h.tile_width > 0x7FFFFFFF ||
        h.tile_height > 0x7FFFFFFF || h.level_mode > 2 || h.rounding > 1) {
      return fail(Status::Decode, "invalid OpenEXR tile description");
    }
    for (const ChunkChannel& c : h.channels) {
      if (c.x_sampling != 1 || c.y_sampling != 1) return fail(Status::Decode, "subsampled channels in a tiled OpenEXR part");
    }
    const auto tiles = [&](std::uint64_t size, std::uint64_t tile) { return (size + tile - 1) / tile; };
    h.tiles_x = tiles(width, h.tile_width);
    h.tiles_y = tiles(height, h.tile_height);
    std::uint64_t total = 0;
    if (h.level_mode == 0) {
      total = h.tiles_x * h.tiles_y;
    } else if (h.level_mode == 1) {
      const std::uint32_t levels = round_log2(std::max(width, height), h.rounding) + 1;
      for (std::uint32_t l = 0; l < levels && total <= max_chunks; ++l) {
        total += tiles(level_size(width, l, h.rounding), h.tile_width) *
                 tiles(level_size(height, l, h.rounding), h.tile_height);
      }
    } else {
      const std::uint32_t levels_x = round_log2(width, h.rounding) + 1;
      const std::uint32_t levels_y = round_log2(height, h.rounding) + 1;
      for (std::uint32_t ly = 0; ly < levels_y && total <= max_chunks; ++ly) {
        for (std::uint32_t lx = 0; lx < levels_x && total <= max_chunks; ++lx) {
          total += tiles(level_size(width, lx, h.rounding), h.tile_width) *
                   tiles(level_size(height, ly, h.rounding), h.tile_height);
        }
      }
    }
    h.computed_chunks = total;
  }
  if (h.computed_chunks > max_chunks) return fail(Status::Decode, "the OpenEXR chunk table does not fit in the file");
  if (h.chunk_count >= 0 && static_cast<std::uint64_t>(h.chunk_count) != h.computed_chunks) {
    return fail(Status::Decode, "the OpenEXR chunk count does not match the image size");
  }
  return {};
}

// --------------------------------------------------------------------------- colour

using Mat3 = std::array<std::array<double, 3>, 3>;

Mat3 multiply(const Mat3& a, const Mat3& b) {
  Mat3 m{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      for (int k = 0; k < 3; ++k) m[i][j] += a[i][k] * b[k][j];
    }
  }
  return m;
}

std::optional<Mat3> inverse(const Mat3& m) {
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (!std::isfinite(det) || std::fabs(det) < 1e-12) return std::nullopt;
  Mat3 r{};
  r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
  r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
  r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
  r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  return r;
}

constexpr std::array<float, 8> kRec709 = {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f};

/// RGB -> XYZ for the given primaries and white point (Y of white = 1).
std::optional<Mat3> rgb_to_xyz(const std::array<float, 8>& c) {
  const auto xyz = [](double x, double y) -> std::array<double, 3> { return {x / y, 1.0, (1.0 - x - y) / y}; };
  for (std::size_t i = 1; i < 8; i += 2) {
    // AP0-style primaries may have negative coordinates; only y == 0 is unusable.
    if (!(std::fabs(c[i]) > 1e-6f) || !std::isfinite(c[i]) || !std::isfinite(c[i - 1])) return std::nullopt;
  }
  const auto r = xyz(c[0], c[1]);
  const auto g = xyz(c[2], c[3]);
  const auto b = xyz(c[4], c[5]);
  const auto w = xyz(c[6], c[7]);
  const Mat3 p = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  const auto inv = inverse(p);
  if (!inv) return std::nullopt;
  std::array<double, 3> s{};
  for (int i = 0; i < 3; ++i) s[i] = (*inv)[i][0] * w[0] + (*inv)[i][1] * w[1] + (*inv)[i][2] * w[2];
  Mat3 m{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m[i][j] = p[i][j] * s[j];
  }
  return m;
}

/// Bradford chromatic adaptation between two white points (xy).
Mat3 bradford(double sx, double sy, double dx, double dy) {
  const Mat3 b = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
  const Mat3 bi = *inverse(b);
  const auto cone = [&](double x, double y) {
    const std::array<double, 3> v = {x / y, 1.0, (1.0 - x - y) / y};
    std::array<double, 3> out{};
    for (int i = 0; i < 3; ++i) out[i] = b[i][0] * v[0] + b[i][1] * v[1] + b[i][2] * v[2];
    return out;
  };
  const auto s = cone(sx, sy);
  const auto d = cone(dx, dy);
  Mat3 scale{};
  for (int i = 0; i < 3; ++i) scale[i][i] = d[i] / s[i];
  return multiply(bi, multiply(scale, b));
}

/// Matrix from the file's RGB to linear Rec.709/sRGB RGB; nullopt when the
/// file already uses Rec.709 primaries (or they are unusable).
std::optional<Mat3> to_rec709(const std::array<float, 8>& c) {
  bool same = true;
  for (std::size_t i = 0; i < 8; ++i) same = same && std::fabs(c[i] - kRec709[i]) < 1e-4f;
  if (same) return std::nullopt;
  const auto src = rgb_to_xyz(c);
  const auto dst = rgb_to_xyz(kRec709);
  if (!src || !dst) return std::nullopt;
  const auto dst_inv = inverse(*dst);
  if (!dst_inv) return std::nullopt;
  Mat3 adapt = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  if (std::fabs(c[6] - kRec709[6]) > 1e-4f || std::fabs(c[7] - kRec709[7]) > 1e-4f) {
    adapt = bradford(c[6], c[7], kRec709[6], kRec709[7]);
  }
  return multiply(*dst_inv, multiply(adapt, *src));
}

// --------------------------------------------------------------------------- part decoding

struct PartPixels {
  ir::HdrPixels pixels;
  bool all_half = true;
  bool grey = false;
  std::size_t missing_chunks = 0;
  std::size_t total_chunks = 0;
  std::string first_error;
  std::vector<std::string> ignored_channels;
};

enum Slot : int { kSlotR = 0, kSlotG = 1, kSlotB = 2, kSlotA = 3 };

struct ChannelPick {
  /// Layout channel index for each of the 4 slots (-1 = absent).
  std::array<int, 4> index{-1, -1, -1, -1};
  bool luma = false;  // slots hold Y, RY, BY, A
};

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

ChannelPick pick_channels(const std::vector<ChunkChannel>& channels, std::vector<std::string>& ignored) {
  static constexpr std::array<std::string_view, 7> kKnown = {"R", "G", "B", "A", "Y", "RY", "BY"};
  const auto split = [](const std::string& name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::size_t{0} : dot + 1;
  };
  // The layer: unprefixed channels when they include a known name, else the
  // first prefix (channels are sorted) that does.
  std::optional<std::string> layer;
  for (const ChunkChannel& c : channels) {
    const std::size_t s = split(c.name);
    const std::string suffix = upper(std::string_view(c.name).substr(s));
    if (std::find(kKnown.begin(), kKnown.end(), suffix) == kKnown.end()) continue;
    const std::string prefix = c.name.substr(0, s);
    if (prefix.empty()) {
      layer = prefix;
      break;
    }
    if (!layer) layer = prefix;
  }
  ChannelPick pick;
  if (!layer) {
    // No colour names at all: show the first channel as grey.
    pick.luma = true;
    pick.index[0] = 0;
  } else {
    std::array<int, 7> found{-1, -1, -1, -1, -1, -1, -1};
    for (std::size_t i = 0; i < channels.size(); ++i) {
      const ChunkChannel& c = channels[i];
      const std::size_t s = split(c.name);
      if (c.name.substr(0, s) != *layer) continue;
      const std::string_view raw = std::string_view(c.name).substr(s);
      const std::string suffix = upper(raw);
      for (std::size_t k = 0; k < kKnown.size(); ++k) {
        // Exact (upper-case) names win over lower-case spellings.
        if (suffix == kKnown[k] && (found[k] < 0 || raw == kKnown[k])) found[k] = static_cast<int>(i);
      }
    }
    if (found[0] >= 0 || found[1] >= 0 || found[2] >= 0) {
      pick.index = {found[0], found[1], found[2], found[3]};
    } else {
      pick.luma = true;
      pick.index = {found[4], found[5], found[6], found[3]};
      if (pick.index[0] < 0 && pick.index[3] < 0) pick.index[0] = 0;
    }
  }
  for (std::size_t i = 0; i < channels.size(); ++i) {
    if (std::find(pick.index.begin(), pick.index.end(), static_cast<int>(i)) == pick.index.end()) {
      ignored.push_back(channels[i].name);
    }
  }
  return pick;
}

float sample_value(PixelType type, const std::byte* p) noexcept {
  switch (type) {
    case PixelType::Half: return exr::half_to_float(exr::load16(p));
    case PixelType::Float: return std::bit_cast<float>(exr::load32(p));
    case PixelType::Uint: return static_cast<float>(exr::load32(p));
  }
  return 0.0f;
}

struct FileView {
  ByteView data;
  bool multipart = false;
  std::size_t table_end = 0;  // first byte after all offset tables
};

/// Read one chunk of a part: validates the chunk header, returns the packed data.
Result<ByteView> read_chunk(const FileView& file, std::uint64_t offset, std::int32_t part, bool tiled,
                            std::array<std::int32_t, 4> expect) {
  if (offset < file.table_end || offset >= file.data.size()) return fail(Status::Decode, "invalid chunk offset");
  util::ByteReader r(file.data);
  r.seek(static_cast<std::size_t>(offset));
  if (file.multipart && r.i32le() != part) return fail(Status::Decode, "chunk belongs to another part");
  if (tiled) {
    const std::int32_t tx = r.i32le();
    const std::int32_t ty = r.i32le();
    const std::int32_t lx = r.i32le();
    const std::int32_t ly = r.i32le();
    if (tx != expect[0] || ty != expect[1] || lx != 0 || ly != 0) return fail(Status::Decode, "unexpected tile");
  } else if (r.i32le() != expect[1]) {
    return fail(Status::Decode, "unexpected scan line chunk");
  }
  const std::int32_t size = r.i32le();
  if (!r.ok() || size < 0 || static_cast<std::size_t>(size) > r.remaining()) {
    return fail(Status::Decode, "chunk is truncated");
  }
  return r.bytes(static_cast<std::size_t>(size));
}

Result<PartPixels> decode_part(const ReadContext& ctx, const FileView& file, const Header& h, std::int32_t part,
                               const std::vector<std::uint64_t>& offsets, std::uint64_t& decompressed,
                               double progress_base, double progress_span) {
  PartPixels result;
  const ChannelPick pick = pick_channels(h.channels, result.ignored_channels);
  for (const int index : pick.index) {
    if (index >= 0 && h.channels[static_cast<std::size_t>(index)].type != PixelType::Half) result.all_half = false;
  }
  const Box& dw = h.data_window;
  const auto dw_w = static_cast<std::uint32_t>(dw.width());
  const auto dw_h = static_cast<std::uint32_t>(dw.height());
  CVT_TRY(ctx.limits.check_pixels(dw_w, dw_h));
  // The best each compression can do, in pixels per byte of file, with
  // slack (DWA codes a flat 8x8 block in a few bits).
  const std::uint64_t best_ratio = h.compression == Compression::None  ? 64
                                   : h.compression == Compression::Rle ? 256
                                   : h.compression == Compression::Dwaa || h.compression == Compression::Dwab
                                       ? std::uint64_t{1} << 15
                                       : 4096;
  CVT_TRY(image::check_plausible(std::uint64_t{dw_w} * dw_h, file.data.size(), best_ratio));
  CVT_TRY(image::check_plausible(static_cast<std::uint64_t>(h.display_window.width()) *
                                     static_cast<std::uint64_t>(h.display_window.height()),
                                 file.data.size(), std::uint64_t{1} << 15));
  const std::size_t pixel_count = std::size_t{dw_w} * dw_h;
  if (pixel_count > SIZE_MAX / 16) return fail(Status::Limit, "the OpenEXR image does not fit in memory");
  const bool has_alpha = pick.index[kSlotA] >= 0;
  // Everything starts transparent black; decoded blocks of a picture without
  // alpha become opaque, so blocks that cannot be read stay transparent.
  std::vector<float> buffer(pixel_count * 4, 0.0f);

  // Chunk list of the full-resolution level.
  struct ChunkRef {
    std::size_t table_index;
    std::int32_t x0, y0, width, height;
    std::array<std::int32_t, 4> expect;
  };
  std::vector<ChunkRef> chunks;
  if (h.tiled) {
    for (std::uint64_t ty = 0; ty < h.tiles_y; ++ty) {
      for (std::uint64_t tx = 0; tx < h.tiles_x; ++tx) {
        const std::uint64_t x = tx * h.tile_width;
        const std::uint64_t y = ty * h.tile_height;
        chunks.push_back({static_cast<std::size_t>(ty * h.tiles_x + tx), static_cast<std::int32_t>(dw.xmin + static_cast<std::int64_t>(x)),
                          static_cast<std::int32_t>(dw.ymin + static_cast<std::int64_t>(y)),
                          static_cast<std::int32_t>(std::min<std::uint64_t>(h.tile_width, dw_w - x)),
                          static_cast<std::int32_t>(std::min<std::uint64_t>(h.tile_height, dw_h - y)),
                          {static_cast<std::int32_t>(tx), static_cast<std::int32_t>(ty), 0, 0}});
      }
    }
  } else {
    const std::int64_t lines = exr::lines_per_chunk(h.compression);
    for (std::uint64_t i = 0; i < h.computed_chunks; ++i) {
      const std::int64_t y = dw.ymin + static_cast<std::int64_t>(i) * lines;
      const auto y0 = static_cast<std::int32_t>(y);
      chunks.push_back({static_cast<std::size_t>(i), dw.xmin, y0, static_cast<std::int32_t>(dw_w),
                        static_cast<std::int32_t>(std::min<std::int64_t>(lines, std::int64_t{dw.ymax} - y + 1)),
                        {0, y0, 0, 0}});
    }
  }
  result.total_chunks = chunks.size();

  std::size_t done = 0;
  for (const ChunkRef& ref : chunks) {
    if ((done++ & 15) == 0) {
      CVT_TRY(ctx.checkpoint(progress_base + progress_span * static_cast<double>(done) / static_cast<double>(chunks.size()),
                             "Decoding"));
    }
    ChunkLayout layout;
    layout.x0 = ref.x0;
    layout.y0 = ref.y0;
    layout.width = ref.width;
    layout.height = ref.height;
    layout.channels = h.channels;
    exr::finish_layout(layout);
    CVT_TRY(ctx.limits.check_decompressed(decompressed + layout.raw_size));
    if (layout.raw_size > SIZE_MAX / 2) return fail(Status::Limit, "OpenEXR chunk is too large");
    auto packed = read_chunk(file, offsets[ref.table_index], part, h.tiled, ref.expect);
    Result<Bytes> raw = packed ? exr::decompress_chunk(h.compression, layout, *packed)
                               : Result<Bytes>(std::unexpected(packed.error()));
    if (!raw) {
      if (raw.error().code != Status::Decode) return std::unexpected(raw.error());
      if (result.first_error.empty()) result.first_error = raw.error().message;
      ++result.missing_chunks;
      continue;
    }
    decompressed += layout.raw_size;
    if (!has_alpha) {
      for (std::int32_t line = 0; line < layout.height; ++line) {
        const auto row = static_cast<std::size_t>(std::int64_t{layout.y0} + line - dw.ymin) * dw_w;
        const auto first = static_cast<std::size_t>(layout.x0 - dw.xmin);
        for (std::size_t x = 0; x < static_cast<std::size_t>(layout.width); ++x) buffer[(row + first + x) * 4 + 3] = 1.0f;
      }
    }
    std::size_t offset = 0;
    for (std::int32_t line = 0; line < layout.height; ++line) {
      const std::int64_t y = std::int64_t{layout.y0} + line;
      for (std::size_t c = 0; c < layout.channels.size(); ++c) {
        const ChunkChannel& channel = layout.channels[c];
        if (!ChunkLayout::has_line(channel, y)) continue;
        const std::size_t size = exr::pixel_size(channel.type);
        const std::size_t row_bytes = static_cast<std::size_t>(channel.width) * size;
        for (std::size_t slot = 0; slot < 4; ++slot) {
          if (pick.index[slot] != static_cast<int>(c)) continue;
          const auto ry = static_cast<std::size_t>(y - dw.ymin);
          const auto first_x = static_cast<std::size_t>(layout.x0 - dw.xmin);
          for (std::size_t k = 0; k < static_cast<std::size_t>(channel.width); ++k) {
            const std::size_t rx = first_x + k * static_cast<std::size_t>(channel.x_sampling);
            buffer[(ry * dw_w + rx) * 4 + slot] = sample_value(channel.type, raw->data() + offset + k * size);
          }
        }
        offset += row_bytes;
      }
    }
  }
  if (result.missing_chunks == chunks.size()) {
    return fail(Status::Decode, "no part of the OpenEXR image could be decoded: {}", result.first_error);
  }

  // Fill in subsampled channels (typically RY/BY) bilinearly.
  for (std::size_t slot = 0; slot < 4; ++slot) {
    if (pick.index[slot] < 0) continue;
    const ChunkChannel& channel = h.channels[static_cast<std::size_t>(pick.index[slot])];
    const auto xs = static_cast<std::size_t>(channel.x_sampling);
    const auto ys = static_cast<std::size_t>(channel.y_sampling);
    if (xs == 1 && ys == 1) continue;
    const std::size_t gw = dw_w / xs;
    const std::size_t gh = dw_h / ys;
    std::vector<float> grid(gw * gh);
    for (std::size_t gy = 0; gy < gh; ++gy) {
      for (std::size_t gx = 0; gx < gw; ++gx) grid[gy * gw + gx] = buffer[((gy * ys) * dw_w + gx * xs) * 4 + slot];
    }
    for (std::size_t y = 0; y < dw_h; ++y) {
      const double v = static_cast<double>(y) / static_cast<double>(ys);
      const std::size_t y0 = std::min(static_cast<std::size_t>(v), gh - 1);
      const std::size_t y1 = std::min(y0 + 1, gh - 1);
      const double ty = std::clamp(v - static_cast<double>(y0), 0.0, 1.0);
      for (std::size_t x = 0; x < dw_w; ++x) {
        const double u = static_cast<double>(x) / static_cast<double>(xs);
        const std::size_t x0 = std::min(static_cast<std::size_t>(u), gw - 1);
        const std::size_t x1 = std::min(x0 + 1, gw - 1);
        const double tx = std::clamp(u - static_cast<double>(x0), 0.0, 1.0);
        const double top = grid[y0 * gw + x0] * (1 - tx) + grid[y0 * gw + x1] * tx;
        const double bottom = grid[y1 * gw + x0] * (1 - tx) + grid[y1 * gw + x1] * tx;
        buffer[(y * dw_w + x) * 4 + slot] = static_cast<float>(top * (1 - ty) + bottom * ty);
      }
    }
  }

  // Luminance/chroma -> RGB, then primaries -> Rec.709.
  const std::array<float, 8> chroma = h.chromaticities.value_or(kRec709);
  std::array<double, 3> yw = {0.2126, 0.7152, 0.0722};
  if (const auto m = rgb_to_xyz(chroma)) {
    const double sum = (*m)[1][0] + (*m)[1][1] + (*m)[1][2];
    if (sum > 0) yw = {(*m)[1][0] / sum, (*m)[1][1] / sum, (*m)[1][2] / sum};
  }
  if (pick.luma) {
    const bool chroma_present = pick.index[1] >= 0 && pick.index[2] >= 0;
    result.grey = !chroma_present;
    for (std::size_t i = 0; i < buffer.size(); i += 4) {
      const float luminance = pick.index[0] >= 0 ? buffer[i] : 0.0f;
      if (!chroma_present || (buffer[i + 1] == 0.0f && buffer[i + 2] == 0.0f)) {
        buffer[i] = buffer[i + 1] = buffer[i + 2] = luminance;
        continue;
      }
      const double r = (buffer[i + 1] + 1.0) * luminance;
      const double b = (buffer[i + 2] + 1.0) * luminance;
      const double g = (luminance - r * yw[0] - b * yw[2]) / yw[1];
      buffer[i] = static_cast<float>(r);
      buffer[i + 1] = static_cast<float>(g);
      buffer[i + 2] = static_cast<float>(b);
    }
  }
  if (const auto m = to_rec709(chroma); m && !result.grey) {
    for (std::size_t i = 0; i < buffer.size(); i += 4) {
      const double r = buffer[i], g = buffer[i + 1], b = buffer[i + 2];
      for (int k = 0; k < 3; ++k) {
        buffer[i + static_cast<std::size_t>(k)] = static_cast<float>((*m)[k][0] * r + (*m)[k][1] * g + (*m)[k][2] * b);
      }
    }
  }
  // OpenEXR colour is premultiplied by alpha; the IR wants straight alpha.
  if (has_alpha) {
    for (std::size_t i = 0; i < buffer.size(); i += 4) {
      const float a = buffer[i + 3];
      if (a > 0.0f && a != 1.0f && std::isfinite(a)) {
        buffer[i] /= a;
        buffer[i + 1] /= a;
        buffer[i + 2] /= a;
      }
    }
  }

  // Place the data window inside the display window.
  const Box& disp = h.display_window;
  ir::HdrPixels& out = result.pixels;
  out.width = static_cast<std::uint32_t>(disp.width());
  out.height = static_cast<std::uint32_t>(disp.height());
  if (dw == disp) {
    out.rgba = std::move(buffer);
    return result;
  }
  out.rgba.assign(std::size_t{out.width} * out.height * 4, 0.0f);
  const std::int64_t x_begin = std::max<std::int64_t>(dw.xmin, disp.xmin);
  const std::int64_t x_end = std::min<std::int64_t>(dw.xmax, disp.xmax);
  const std::int64_t y_begin = std::max<std::int64_t>(dw.ymin, disp.ymin);
  const std::int64_t y_end = std::min<std::int64_t>(dw.ymax, disp.ymax);
  for (std::int64_t y = y_begin; y <= y_end; ++y) {
    if (x_begin > x_end) break;
    const auto src = static_cast<std::size_t>(((y - dw.ymin) * dw_w + (x_begin - dw.xmin)) * 4);
    const auto dst = static_cast<std::size_t>(((y - disp.ymin) * out.width + (x_begin - disp.xmin)) * 4);
    std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(src), static_cast<std::size_t>(x_end - x_begin + 1) * 4,
                out.rgba.begin() + static_cast<std::ptrdiff_t>(dst));
  }
  return result;
}

image::ToneMapOptions tone_map_options(const Options& options) {
  image::ToneMapOptions tm;
  const std::string op = options.get_enum("tone_map", "auto", {"auto", "clamp", "reinhard", "filmic"});
  tm.op = op == "clamp"      ? image::ToneMapOperator::Clamp
          : op == "reinhard" ? image::ToneMapOperator::Reinhard
          : op == "filmic"   ? image::ToneMapOperator::Filmic
                             : image::ToneMapOperator::Auto;
  tm.exposure_stops = static_cast<float>(options.get_float("exposure", 0.0, -16.0, 16.0));
  return tm;
}

Result<ir::Image> read_exr(ReadContext& ctx) {
  const ByteView data = ctx.input;
  util::ByteReader r(data);
  if (r.u32le() != kMagic) return fail(Status::Decode, "not an OpenEXR file");
  const std::uint32_t version = r.u32le();
  if (!r.ok()) return fail(Status::Decode, "the OpenEXR file is truncated");
  if ((version & 0xFF) != 2) return fail(Status::Decode, "unsupported OpenEXR version {}", version & 0xFF);
  const std::uint32_t flags = version & ~0xFFu;
  if ((flags & ~(kTiledFlag | kLongNamesFlag | kDeepFlag | kMultipartFlag)) != 0) {
    return fail(Status::Decode, "the OpenEXR file uses unknown features");
  }
  const bool multipart = (flags & kMultipartFlag) != 0;
  if (!multipart && (flags & kDeepFlag) != 0) return fail(Status::Decode, "deep OpenEXR images are not supported");

  std::vector<Header> headers;
  if (multipart) {
    while (true) {
      if (r.peek_u8(0xFF) == 0) {
        r.skip(1);
        break;
      }
      if (headers.size() >= 1024) return fail(Status::Decode, "too many OpenEXR parts");
      CVT_TRY_ASSIGN(Header header, parse_header(r));
      headers.push_back(std::move(header));
      if (!r.ok()) return fail(Status::Decode, "the OpenEXR header is truncated");
    }
    if (!r.ok() || headers.empty()) return fail(Status::Decode, "the OpenEXR header is truncated");
  } else {
    CVT_TRY_ASSIGN(Header header, parse_header(r));
    headers.push_back(std::move(header));
  }
  const std::uint64_t max_chunks = r.remaining() / 8;
  std::uint64_t table_entries = 0;
  for (Header& header : headers) {
    if (multipart && header.chunk_count < 0) return fail(Status::Decode, "OpenEXR part without a chunk count");
    CVT_TRY(validate_part(header, (flags & kTiledFlag) != 0, max_chunks));
    const std::uint64_t entries =
        header.deep ? static_cast<std::uint64_t>(header.chunk_count) : header.computed_chunks;
    table_entries += entries;
    if (table_entries > max_chunks) return fail(Status::Decode, "the OpenEXR chunk table does not fit in the file");
  }
  std::vector<std::vector<std::uint64_t>> offsets(headers.size());
  for (std::size_t p = 0; p < headers.size(); ++p) {
    const std::uint64_t entries =
        headers[p].deep ? static_cast<std::uint64_t>(headers[p].chunk_count) : headers[p].computed_chunks;
    offsets[p].resize(static_cast<std::size_t>(entries));
    for (std::uint64_t& offset : offsets[p]) offset = r.u64le();
  }
  if (!r.ok()) return fail(Status::Decode, "the OpenEXR chunk table is truncated");
  const FileView file{data, multipart, r.position()};

  // Parts to decode: the first image part, plus later parts of the same size.
  std::vector<std::size_t> parts;
  std::size_t skipped_deep = 0;
  std::size_t skipped_size = 0;
  for (std::size_t p = 0; p < headers.size(); ++p) {
    if (headers[p].deep) {
      ++skipped_deep;
      continue;
    }
    if (!parts.empty()) {
      const Box& a = headers[parts[0]].display_window;
      const Box& b = headers[p].display_window;
      if (a.width() != b.width() || a.height() != b.height()) {
        ++skipped_size;
        continue;
      }
    }
    parts.push_back(p);
  }
  if (parts.empty()) return fail(Status::Decode, "the OpenEXR file holds only deep images, which are not supported");
  const Box& first_display = headers[parts[0]].display_window;
  CVT_TRY(image::check_size(ctx, static_cast<std::uint64_t>(first_display.width()),
                            static_cast<std::uint64_t>(first_display.height()), parts.size()));
  if (skipped_deep > 0) ctx.warn(std::format("{} deep OpenEXR part(s) were skipped.", skipped_deep));
  if (skipped_size > 0) {
    ctx.warn(std::format("{} OpenEXR part(s) with a different picture size were skipped.", skipped_size));
  }

  const image::ToneMapOptions tm = tone_map_options(ctx.options);
  ir::Image image;
  bool all_half = true;
  std::uint64_t decompressed = 0;
  std::size_t missing = 0;
  std::size_t total = 0;
  std::vector<std::string> ignored;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const Header& header = headers[parts[i]];
    const double span = 1.0 / static_cast<double>(parts.size());
    CVT_TRY_ASSIGN(PartPixels part, decode_part(ctx, file, header, static_cast<std::int32_t>(parts[i]),
                                                offsets[parts[i]], decompressed, span * static_cast<double>(i), span));
    all_half = all_half && part.all_half;
    missing += part.missing_chunks;
    total += part.total_chunks;
    if (i == 0) {
      ignored = part.ignored_channels;
      image.source_grayscale = part.grey;
    }
    image.frames.push_back(image::tone_map(part.pixels, tm));
    if (i == 0) image.hdr = std::move(part.pixels);
  }
  if (missing > 0) {
    ctx.warn(std::format("The OpenEXR file is truncated or damaged; {} of {} blocks could not be read and were left "
                         "transparent.",
                         missing, total));
  }
  if (!ignored.empty()) {
    std::string list;
    for (std::size_t i = 0; i < ignored.size() && i < 8; ++i) list += (i == 0 ? "" : ", ") + ignored[i];
    if (ignored.size() > 8) list += ", ...";
    ctx.warn(std::format("Only the colour channels were converted; {} other channel(s) were ignored ({}).",
                         ignored.size(), list));
  }
  image.source_bit_depth = all_half ? 16 : 32;
  return image;
}

// --------------------------------------------------------------------------- writing

void put_string(util::ByteWriter& w, std::string_view s) { w.text(s).u8(0); }

void put_attribute(util::ByteWriter& w, std::string_view name, std::string_view type, ByteView value) {
  put_string(w, name);
  put_string(w, type);
  w.i32le(static_cast<std::int32_t>(value.size()));
  w.bytes(value);
}

Result<Bytes> write_exr(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  image::warn_if_animated(*prepared, ctx, "OpenEXR");
  const ir::Image& img = *prepared;
  const std::uint32_t w = img.width();
  const std::uint32_t h = img.height();
  const ir::HdrPixels pixels =
      img.hdr && img.hdr->width == w && img.hdr->height == h && img.hdr->rgba.size() == std::size_t{w} * h * 4
          ? *img.hdr
          : image::to_linear(img.frames.front());

  const std::string compression_name =
      ctx.options.get_enum("compression", "zip", {"none", "rle", "zips", "zip", "piz"});
  const Compression compression = compression_name == "none"   ? Compression::None
                                  : compression_name == "rle"  ? Compression::Rle
                                  : compression_name == "zips" ? Compression::Zips
                                  : compression_name == "piz"  ? Compression::Piz
                                                               : Compression::Zip;
  const PixelType type =
      ctx.options.get_enum("pixel_type", "half", {"half", "float"}) == "float" ? PixelType::Float : PixelType::Half;
  const int level = options::resolved_compression_level(ctx.options);

  bool alpha = false;
  bool grey = true;
  for (std::size_t i = 0; i < pixels.rgba.size(); i += 4) {
    if (pixels.rgba[i + 3] != 1.0f) alpha = true;
    if (pixels.rgba[i] != pixels.rgba[i + 1] || pixels.rgba[i] != pixels.rgba[i + 2]) grey = false;
  }
  // Channel names in the required alphabetical order, each with the RGBA
  // component it stores.
  std::vector<std::pair<std::string, std::size_t>> channels;
  if (alpha) channels.emplace_back("A", 3);
  if (grey) {
    channels.emplace_back("Y", 0);
  } else {
    channels.emplace_back("B", 2);
    channels.emplace_back("G", 1);
    channels.emplace_back("R", 0);
  }

  util::ByteWriter out;
  out.u32le(kMagic).u32le(2);
  {
    util::ByteWriter list;
    for (const auto& [name, component] : channels) {
      (void)component;
      put_string(list, name);
      list.i32le(static_cast<std::int32_t>(type)).u8(0).u8(0).u8(0).u8(0).i32le(1).i32le(1);
    }
    list.u8(0);
    put_attribute(out, "channels", "chlist", list.view());
  }
  const std::byte compression_byte{static_cast<std::uint8_t>(compression)};
  put_attribute(out, "compression", "compression", ByteView(&compression_byte, 1));
  util::ByteWriter box;
  box.i32le(0).i32le(0).i32le(static_cast<std::int32_t>(w - 1)).i32le(static_cast<std::int32_t>(h - 1));
  put_attribute(out, "dataWindow", "box2i", box.view());
  put_attribute(out, "displayWindow", "box2i", box.view());
  const std::byte line_order{0};
  put_attribute(out, "lineOrder", "lineOrder", ByteView(&line_order, 1));
  util::ByteWriter one;
  one.f32le(1.0f);
  put_attribute(out, "pixelAspectRatio", "float", one.view());
  util::ByteWriter center;
  center.f32le(0.0f).f32le(0.0f);
  put_attribute(out, "screenWindowCenter", "v2f", center.view());
  put_attribute(out, "screenWindowWidth", "float", one.view());
  out.u8(0);

  const auto lines = static_cast<std::uint32_t>(exr::lines_per_chunk(compression));
  const std::uint32_t chunk_count = (h + lines - 1) / lines;
  const std::size_t table_at = out.size();
  out.zeros(std::size_t{chunk_count} * 8);
  const std::size_t sample_size = exr::pixel_size(type);
  Bytes raw;
  for (std::uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(chunk) / chunk_count, "Encoding"));
    const std::uint32_t y0 = chunk * lines;
    const std::uint32_t rows = std::min(lines, h - y0);
    ChunkLayout layout;
    layout.x0 = 0;
    layout.y0 = static_cast<std::int32_t>(y0);
    layout.width = static_cast<std::int32_t>(w);
    layout.height = static_cast<std::int32_t>(rows);
    for (const auto& [name, component] : channels) {
      (void)component;
      ChunkChannel channel;
      channel.name = name;
      channel.type = type;
      layout.channels.push_back(channel);
    }
    exr::finish_layout(layout);
    raw.assign(static_cast<std::size_t>(layout.raw_size), std::byte{0});
    std::byte* p = raw.data();
    for (std::uint32_t y = y0; y < y0 + rows; ++y) {
      const float* row = pixels.rgba.data() + std::size_t{y} * w * 4;
      for (const auto& [name, component] : channels) {
        (void)name;
        for (std::uint32_t x = 0; x < w; ++x, p += sample_size) {
          const float* px = row + std::size_t{x} * 4;
          float a = std::isfinite(px[3]) ? std::clamp(px[3], 0.0f, 1.0f) : 1.0f;
          float v = component == 3 ? a : px[component] * a;  // premultiplied, as OpenEXR expects
          if (!std::isfinite(v)) v = std::isnan(v) ? 0.0f : v;
          if (type == PixelType::Half) {
            v = std::clamp(v, -65504.0f, 65504.0f);
            exr::store16(p, exr::float_to_half(v));
          } else {
            exr::store32(p, std::bit_cast<std::uint32_t>(v));
          }
        }
      }
    }
    Bytes packed;
    if (compression != Compression::None) {
      CVT_TRY_ASSIGN(packed, exr::compress_chunk(compression, layout, raw, level));
    }
    const ByteView stored = compression == Compression::None || packed.size() >= raw.size() ? ByteView(raw)
                                                                                           : ByteView(packed);
    const std::uint64_t offset = out.size();
    out.patch(table_at + std::size_t{chunk} * 8, offset, std::endian::little);
    out.i32le(static_cast<std::int32_t>(y0));
    out.i32le(static_cast<std::int32_t>(stored.size()));
    out.bytes(stored);
    CVT_TRY(ctx.limits.check_output(out.size()));
  }
  return out.take();
}

}  // namespace

void register_exr(Registry& registry) {
  FormatInfo info;
  info.id = "exr";
  info.name = "OpenEXR image";
  info.description = "High dynamic range image used in film, visual effects and 3D rendering.";
  info.category = Category::Image;
  info.extensions = {"exr"};
  info.mime = "image/x-exr";
  info.popularity = 30;
  info.signatures = {Signature::at(0, "76 2F 31 01")};
  registry.add_format(std::move(info));

  image::ImageCodecSpec spec;
  spec.format_id = "exr";
  spec.read = image::reader(read_exr);
  spec.write = image::writer(write_exr);
  spec.write_options = image::with(
      image::lossless_writer_options(9),
      {
          OptionSpec::choice("compression", "Compression", "zip", {"zip", "piz", "zips", "rle", "none"},
                             "Lossless compression method: ZIP (best for renders and graphics, widely supported), "
                             "PIZ (best for noisy or photographic content), ZIPS (one line at a time), RLE or none."),
          OptionSpec::choice("pixel_type", "Sample precision", "half", {"half", "float"},
                             "16-bit half floats (the usual choice, half the size) or full 32-bit floats."),
      });
  spec.read_options = {
      OptionSpec::choice("tone_map", "Tone mapping", "auto", {"auto", "clamp", "reinhard", "filmic"},
                         "How HDR brightness is mapped to the normal range for formats that cannot store it.")
          .advanced(),
      OptionSpec::number("exposure", "Exposure", 0.0, -16.0, 16.0,
                         "Brightness adjustment in stops before tone mapping.")
          .advanced(),
  };
  image::add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image_modern
