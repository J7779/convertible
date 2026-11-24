// Convertible core - image family: GIF (87a/89a).
//
// Reader: global and local colour tables, interlacing, LZW with every code
// size, graphic control extensions (delay, transparency, all disposal
// methods), NETSCAPE/ANIMEXTS loop counts, comments. Frames are composited
// onto the logical screen the way browsers do (transparent background).
//
// Writer: exact palettes when the picture has at most 256 colours,
// otherwise median-cut + k-means quantisation with optional dithering;
// global or per-frame palettes; animations are written with frame
// differencing (changed rectangle only, unchanged pixels transparent) and
// the right disposal methods.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/image_ops.hpp"
#include "util/bytes.hpp"
#include "util/text.hpp"

namespace cvt::image {

namespace {

// --------------------------------------------------------------------------- LZW decoding

/// Decode GIF LZW data (already de-blocked) into `out` (exactly out.size()
/// indices; missing pixels keep their value). Returns false on corrupt data.
bool lzw_decode(ByteView data, int min_code_size, std::vector<std::uint8_t>& out) {
  if (min_code_size < 2 || min_code_size > 11) return false;
  const int clear = 1 << min_code_size;
  const int end = clear + 1;
  std::array<std::uint16_t, 4096> prefix{};
  std::array<std::uint8_t, 4096> suffix{};
  std::array<std::uint8_t, 4096> first{};
  for (int i = 0; i < clear; ++i) {
    suffix[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    first[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  }
  int code_size = min_code_size + 1;
  int next = end + 1;
  int previous = -1;
  std::uint32_t acc = 0;
  int bits = 0;
  std::size_t in = 0;
  std::size_t pos = 0;
  const std::size_t total = out.size();
  std::vector<std::uint8_t> stack(4096);
  while (pos < total) {
    while (bits < code_size) {
      if (in >= data.size()) return pos > 0;
      acc |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[in++])) << bits;
      bits += 8;
    }
    const int code = static_cast<int>(acc & ((1u << code_size) - 1u));
    acc >>= code_size;
    bits -= code_size;
    if (code == clear) {
      code_size = min_code_size + 1;
      next = end + 1;
      previous = -1;
      continue;
    }
    if (code == end) return true;
    int current = code;
    std::uint8_t first_char = 0;
    if (previous < 0) {
      if (code >= clear) return false;
      out[pos++] = static_cast<std::uint8_t>(code);
      previous = code;
      continue;
    }
    std::size_t sp = 0;
    if (code > next || (code == next && next >= 4096)) return false;
    if (code == next) {
      // KwKwK case: the string of `previous` plus its own first character.
      first_char = first[static_cast<std::size_t>(previous)];
      stack[sp++] = first_char;
      current = previous;
    }
    while (current >= clear) {
      if (sp >= stack.size()) return false;
      stack[sp++] = suffix[static_cast<std::size_t>(current)];
      current = prefix[static_cast<std::size_t>(current)];
    }
    if (sp >= stack.size()) return false;
    stack[sp++] = static_cast<std::uint8_t>(current);
    first_char = static_cast<std::uint8_t>(current);
    while (sp > 0 && pos < total) out[pos++] = stack[--sp];
    if (next < 4096) {
      prefix[static_cast<std::size_t>(next)] = static_cast<std::uint16_t>(previous);
      suffix[static_cast<std::size_t>(next)] = first_char;
      first[static_cast<std::size_t>(next)] = first[static_cast<std::size_t>(previous)];
      ++next;
      if (next == (1 << code_size) && code_size < 12) ++code_size;
    }
    previous = code;
  }
  return true;
}

/// Concatenate data sub-blocks starting at the reader position.
Result<Bytes> read_sub_blocks(util::ByteReader& reader, const Context& ctx, bool& truncated) {
  Bytes out;
  truncated = false;
  while (true) {
    const std::uint8_t size = reader.u8();
    if (!reader.ok()) {
      truncated = true;
      break;
    }
    if (size == 0) break;
    const ByteView block = reader.bytes(size);
    if (!reader.ok()) {
      truncated = true;
      break;
    }
    CVT_TRY(ctx.limits.check_decompressed(out.size() + size));
    append(out, block);
  }
  return out;
}

void skip_sub_blocks(util::ByteReader& reader) {
  while (reader.ok()) {
    const std::uint8_t size = reader.u8();
    if (size == 0 || !reader.ok()) return;
    reader.skip(size);
  }
}

using Palette = std::vector<Rgba>;

Palette read_palette(util::ByteReader& reader, int entries) {
  Palette palette(static_cast<std::size_t>(entries));
  for (Rgba& c : palette) {
    c[0] = reader.u8();
    c[1] = reader.u8();
    c[2] = reader.u8();
    c[3] = 255;
  }
  return palette;
}

Result<ir::Image> read_gif(ReadContext& ctx) {
  util::ByteReader reader(ctx.input);
  const std::string_view magic = reader.chars(6);
  if (magic != "GIF87a" && magic != "GIF89a") return fail(Status::Decode, "not a GIF file");
  const std::uint32_t width = reader.u16le();
  const std::uint32_t height = reader.u16le();
  const std::uint8_t flags = reader.u8();
  reader.skip(2);  // background index, aspect ratio
  if (!reader.ok()) return fail(Status::Decode, "GIF header is truncated");
  CVT_TRY(check_size(ctx, width, height));
  // LZW cannot produce more than a few thousand pixels per byte.
  CVT_TRY(check_plausible(std::uint64_t{width} * height, ctx.input.size(), 4096));
  Palette global;
  if (flags & 0x80) {
    global = read_palette(reader, 2 << (flags & 7));
    if (!reader.ok()) return fail(Status::Decode, "GIF colour table is truncated");
  }
  ir::Image image;
  image.loop_count = 1;
  std::vector<std::uint8_t> canvas(static_cast<std::size_t>(width) * height * 4, 0);
  std::vector<std::uint8_t> saved;
  // Pending graphic control.
  int disposal = 0;
  int transparent = -1;
  std::uint32_t delay_cs = 0;
  bool truncated_any = false;
  std::size_t frame_count = 0;
  while (reader.ok() && !reader.at_end()) {
    const std::uint8_t introducer = reader.u8();
    if (introducer == 0x3B) break;  // trailer
    if (introducer == 0x21) {
      const std::uint8_t label = reader.u8();
      if (label == 0xF9) {
        const std::uint8_t size = reader.u8();
        const ByteView body = reader.bytes(size);
        if (!reader.ok()) break;
        if (size >= 4) {
          const auto packed = std::to_integer<std::uint8_t>(body[0]);
          disposal = (packed >> 2) & 7;
          delay_cs = std::to_integer<std::uint32_t>(body[1]) | std::to_integer<std::uint32_t>(body[2]) << 8;
          transparent = (packed & 1) ? std::to_integer<int>(body[3]) : -1;
        }
        skip_sub_blocks(reader);
      } else if (label == 0xFF) {
        const std::uint8_t size = reader.u8();
        const std::string_view app = reader.chars(size);
        bool truncated = false;
        CVT_TRY_ASSIGN(const Bytes body, read_sub_blocks(reader, ctx, truncated));
        if ((app == "NETSCAPE2.0" || app == "ANIMEXTS1.0") && body.size() >= 3 && body[0] == std::byte{1}) {
          // The stored value counts repeats after the first play (0 = forever).
          const std::uint32_t repeats = std::to_integer<std::uint32_t>(body[1]) | std::to_integer<std::uint32_t>(body[2]) << 8;
          image.loop_count = repeats == 0 ? 0 : repeats + 1;
        } else if (app == "XMP DataXMP" && !body.empty()) {
          // XMP is stored raw with a 257-byte "magic trailer" that the
          // sub-block framing turns into part of the data.
          std::string xmp = to_string(body);
          if (xmp.size() > 257) xmp.resize(xmp.size() - 257);
          image.xmp = util::sanitize_utf8(xmp);
        }
      } else if (label == 0xFE) {
        bool truncated = false;
        CVT_TRY_ASSIGN(const Bytes body, read_sub_blocks(reader, ctx, truncated));
        if (!body.empty()) image.metadata["Comment"] = util::decode_text(body, util::TextEncoding::Latin1).text;
      } else {
        skip_sub_blocks(reader);
      }
      continue;
    }
    if (introducer != 0x2C) {
      if (frame_count > 0) break;  // garbage after the last frame: ignore like browsers
      return fail(Status::Decode, "GIF file has an unknown block (0x{:02X})", introducer);
    }
    const std::uint32_t left = reader.u16le();
    const std::uint32_t top = reader.u16le();
    const std::uint32_t fw = reader.u16le();
    const std::uint32_t fh = reader.u16le();
    const std::uint8_t fflags = reader.u8();
    if (!reader.ok()) break;
    Palette local;
    if (fflags & 0x80) {
      local = read_palette(reader, 2 << (fflags & 7));
      if (!reader.ok()) break;
    }
    const Palette& palette = local.empty() ? global : local;
    const int min_code = reader.u8();
    bool truncated = false;
    CVT_TRY_ASSIGN(const Bytes lzw, read_sub_blocks(reader, ctx, truncated));
    truncated_any = truncated_any || truncated;
    ++frame_count;
    CVT_TRY(ctx.limits.check_pixels(width, height, frame_count));
    CVT_TRY(check_plausible(std::uint64_t{fw} * fh, lzw.size(), 4096));
    if (frame_count > ctx.limits.max_archive_entries) return fail(Status::Limit, "the GIF has too many frames");
    CVT_TRY(ctx.checkpoint(std::min(0.99, static_cast<double>(reader.position()) / static_cast<double>(ctx.input.size())), "Decoding"));
    if (fw == 0 || fh == 0) {
      // An empty frame still shows the canvas for its delay.
    }
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(fw) * fh,
                                      static_cast<std::uint8_t>(transparent >= 0 ? transparent : 0));
    if (!indices.empty() && !lzw_decode(lzw, min_code, indices)) {
      if (image.frames.empty() && frame_count == 1 && lzw.empty()) return fail(Status::Decode, "GIF image data is corrupt");
      ctx.warn("The GIF image data is damaged; parts of a frame may be wrong.");
    }
    // De-interlace.
    if ((fflags & 0x40) && fh > 1) {
      std::vector<std::uint8_t> ordered(indices.size());
      std::uint32_t row = 0;
      const std::uint32_t starts[4] = {0, 4, 2, 1};
      const std::uint32_t steps[4] = {8, 8, 4, 2};
      for (int pass = 0; pass < 4; ++pass) {
        for (std::uint32_t y = starts[pass]; y < fh; y += steps[pass], ++row) {
          ::cvt::util::copy_bytes(ordered.data() + static_cast<std::size_t>(y) * fw, indices.data() + static_cast<std::size_t>(row) * fw, fw);
        }
      }
      indices = std::move(ordered);
    }
    if (disposal == 3) saved = canvas;
    for (std::uint32_t y = 0; y < fh; ++y) {
      const std::uint32_t cy = top + y;
      if (cy >= height) break;
      for (std::uint32_t x = 0; x < fw; ++x) {
        const std::uint32_t cx = left + x;
        if (cx >= width) break;
        const std::uint8_t index = indices[static_cast<std::size_t>(y) * fw + x];
        if (static_cast<int>(index) == transparent) continue;
        std::uint8_t* p = canvas.data() + (static_cast<std::size_t>(cy) * width + cx) * 4;
        if (index < palette.size()) {
          ::cvt::util::copy_bytes(p, palette[index].data(), 4);
        } else {
          p[0] = p[1] = p[2] = 0;
          p[3] = 255;
        }
      }
    }
    ir::Frame frame;
    frame.width = width;
    frame.height = height;
    frame.rgba = canvas;
    frame.delay_ms = delay_cs * 10;
    image.frames.push_back(std::move(frame));
    if (disposal == 2) {
      for (std::uint32_t y = top; y < std::min(height, top + fh); ++y) {
        if (left >= width) break;
        ::cvt::util::fill_bytes(canvas.data() + (static_cast<std::size_t>(y) * width + left) * 4, 0,
                    static_cast<std::size_t>(std::min(width - left, fw)) * 4);
      }
    } else if (disposal == 3 && !saved.empty()) {
      canvas = saved;
    }
    disposal = 0;
    transparent = -1;
    delay_cs = 0;
    if (truncated) break;
  }
  if (image.frames.empty()) return fail(Status::Decode, "GIF file contains no picture");
  if (truncated_any) ctx.warn("The GIF file is truncated.");
  if (image.frames.size() == 1) image.frames[0].delay_ms = 0;
  return image;
}

// --------------------------------------------------------------------------- LZW encoding

void lzw_encode(std::span<const std::uint8_t> indices, int min_code_size, util::ByteWriter& out) {
  const int clear = 1 << min_code_size;
  const int end = clear + 1;
  int code_size = min_code_size + 1;
  int next = end + 1;
  // Hash table: (prefix << 8 | byte) -> code.
  constexpr std::size_t table_size = 1 << 13;
  std::vector<std::int32_t> keys(table_size, -1);
  std::vector<std::uint16_t> codes(table_size, 0);
  std::vector<std::uint8_t> block;
  block.reserve(255);
  std::uint32_t acc = 0;
  int bits = 0;
  const auto flush_block = [&]() {
    if (block.empty()) return;
    out.u8(static_cast<std::uint8_t>(block.size()));
    out.bytes(std::span<const std::uint8_t>(block));
    block.clear();
  };
  const auto emit = [&](int code) {
    acc |= static_cast<std::uint32_t>(code) << bits;
    bits += code_size;
    while (bits >= 8) {
      block.push_back(static_cast<std::uint8_t>(acc));
      acc >>= 8;
      bits -= 8;
      if (block.size() == 255) flush_block();
    }
  };
  const auto reset = [&]() {
    std::fill(keys.begin(), keys.end(), -1);
    code_size = min_code_size + 1;
    next = end + 1;
  };
  emit(clear);
  if (indices.empty()) {
    emit(end);
  } else {
    int prefix = indices[0];
    for (std::size_t i = 1; i < indices.size(); ++i) {
      const int c = indices[i];
      const std::int32_t key = prefix << 8 | c;
      std::size_t slot = (static_cast<std::uint32_t>(key) * 2654435761u >> 19) & (table_size - 1);
      bool found = false;
      while (keys[slot] != -1) {
        if (keys[slot] == key) {
          found = true;
          break;
        }
        slot = (slot + 1) & (table_size - 1);
      }
      if (found) {
        prefix = codes[slot];
        continue;
      }
      emit(prefix);
      if (next < 4096) {
        keys[slot] = key;
        codes[slot] = static_cast<std::uint16_t>(next);
        // The decoder widens its codes once `next` reaches 2^code_size.
        if (next == (1 << code_size) && code_size < 12) ++code_size;
        ++next;
      } else {
        emit(clear);
        reset();
      }
      prefix = c;
    }
    emit(prefix);
    // The decoder adds one more entry after reading `prefix`, which can widen the code.
    if (next < 4096 && next == (1 << code_size) && code_size < 12) ++code_size;
    emit(end);
  }
  if (bits > 0) block.push_back(static_cast<std::uint8_t>(acc));
  flush_block();
  out.u8(0);
}

// --------------------------------------------------------------------------- writing

struct Rect {
  std::uint32_t x = 0, y = 0, w = 0, h = 0;
};

struct EncodedFrame {
  Rect rect;
  std::vector<Rgba> local_palette;  // empty = global
  int transparent = -1;
  int disposal = 1;
  std::uint32_t delay_cs = 0;
  std::vector<std::uint8_t> indices;  // rect.w * rect.h
  int clear = -1;                      // the palette's transparent entry, if any
};

int bits_for(std::size_t colors) {
  int bits = 1;
  while ((std::size_t{1} << bits) < colors) ++bits;
  return bits;
}

void write_palette(util::ByteWriter& out, const std::vector<Rgba>& palette) {
  const std::size_t size = std::size_t{1} << bits_for(std::max<std::size_t>(palette.size(), 2));
  for (std::size_t i = 0; i < size; ++i) {
    const Rgba c = i < palette.size() ? palette[i] : Rgba{0, 0, 0, 255};
    out.u8(c[0]).u8(c[1]).u8(c[2]);
  }
}

Result<Bytes> write_gif(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  const ir::Image& image = *prepared;
  const std::uint32_t width = image.width();
  const std::uint32_t height = image.height();
  if (width > 65535 || height > 65535) return fail(Status::Encode, "GIF pictures are limited to 65535x65535 pixels");
  const int quality = options::resolved_quality(ctx.options);
  QuantizeOptions qo;
  qo.max_colors = quality >= 70 ? 256 : quality >= 50 ? 128 : quality >= 30 ? 64 : quality >= 10 ? 32 : 16;
  qo.dither = resolved_dither(ctx.options);
  qo.keep_alpha = false;  // GIF transparency is binary
  qo.refine_iterations = 4;
  const std::string palette_mode = ctx.options.get_enum("palette", "auto", {"auto", "global", "local"});

  // Frames with binary alpha (threshold 128), the way they will be shown.
  std::vector<ir::Frame> frames;
  frames.reserve(image.frames.size());
  bool partial_alpha = false;
  for (const ir::Frame& f : image.frames) {
    ir::Frame copy = f;
    for (std::size_t i = 3; i < copy.rgba.size(); i += 4) {
      if (copy.rgba[i] != 0 && copy.rgba[i] != 255) partial_alpha = true;
      copy.rgba[i] = copy.rgba[i] >= 128 ? 255 : 0;
      if (copy.rgba[i] == 0) copy.rgba[i - 3] = copy.rgba[i - 2] = copy.rgba[i - 1] = 0;
    }
    frames.push_back(std::move(copy));
  }
  if (partial_alpha) ctx.warn("GIF transparency is on or off; partly transparent pixels were made opaque or clear.");
  const bool animated = frames.size() > 1;

  // Palette: exact when every frame together has at most 255 colours (one
  // slot stays free for transparency in animations), else quantised.
  std::vector<const ir::Frame*> pointers;
  for (const ir::Frame& f : frames) pointers.push_back(&f);
  std::vector<Rgba> global;
  bool use_local = palette_mode == "local";
  bool exact = false;
  {
    std::vector<Rgba> colors;
    bool fits = true;
    for (const ir::Frame& f : frames) {
      const auto q = exact_palette(f, 256);
      if (!q) {
        fits = false;
        break;
      }
      for (const Rgba& c : q->palette) {
        if (std::find(colors.begin(), colors.end(), c) == colors.end()) colors.push_back(c);
      }
      if (colors.size() > 256) {
        fits = false;
        break;
      }
    }
    const bool has_clear = std::find(colors.begin(), colors.end(), Rgba{0, 0, 0, 0}) != colors.end();
    if (fits && (colors.size() < 256 || (!animated) || has_clear)) {
      global = colors;
      exact = true;
      use_local = false;
    }
  }
  if (!exact) {
    if (palette_mode == "auto") use_local = animated;
    if (!use_local) {
      QuantizeOptions shared = qo;
      if (animated) shared.reserved = Rgba{0, 0, 0, 0};
      global = build_palette(pointers, shared);
    }
  }
  // Animations need a transparent slot for frame differencing.
  const auto ensure_clear = [&](std::vector<Rgba>& palette) -> int {
    for (std::size_t i = 0; i < palette.size(); ++i) {
      if (palette[i][3] == 0) return static_cast<int>(i);
    }
    if (palette.size() < 256) {
      palette.push_back({0, 0, 0, 0});
      return static_cast<int>(palette.size() - 1);
    }
    return -1;
  };
  int global_clear = -1;
  if (!use_local && (animated || exact)) {
    global_clear = ensure_clear(global);
  } else if (!use_local) {
    for (std::size_t i = 0; i < global.size(); ++i) {
      if (global[i][3] == 0) global_clear = static_cast<int>(i);
    }
  }

  // Encode the frames with differencing against what is on screen.
  std::vector<EncodedFrame> encoded;
  std::vector<std::uint8_t> screen(static_cast<std::size_t>(width) * height * 4, 0);
  std::vector<std::uint8_t> previous_full;  // palette indices of the previous frame, whole canvas
  for (std::size_t n = 0; n < frames.size(); ++n) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(n) / static_cast<double>(frames.size()), "Encoding"));
    const ir::Frame& frame = frames[n];
    std::vector<Rgba> palette = global;
    int clear = global_clear;
    if (use_local) {
      QuantizeOptions local = qo;
      local.reserved = Rgba{0, 0, 0, 0};
      const ir::Frame* one[1] = {&frame};
      palette = build_palette(one, local);
      clear = ensure_clear(palette);
    }
    QuantizeOptions mapping = qo;
    if (exact) mapping.dither = Dither::None;
    mapping.dither_strength = 1.0f;
    CVT_TRY_ASSIGN(Quantized q, remap(frame, palette, mapping));
    // What this frame shows, as colours.
    std::vector<std::uint8_t> shown(screen.size());
    for (std::size_t i = 0; i < q.indices.size(); ++i) ::cvt::util::copy_bytes(shown.data() + i * 4, palette[q.indices[i]].data(), 4);
    EncodedFrame ef;
    ef.delay_cs = (frame.delay_ms + 5) / 10;
    ef.transparent = clear;
    ef.clear = clear;
    ef.local_palette = use_local ? palette : std::vector<Rgba>{};
    Rect rect{0, 0, width, height};
    bool need_clear_previous = false;
    if (n > 0) {
      // Pixels that must become transparent cannot be drawn over: the previous
      // frame is then disposed to background first.
      for (std::size_t i = 0; i < q.indices.size(); ++i) {
        if (shown[i * 4 + 3] == 0 && screen[i * 4 + 3] != 0) {
          need_clear_previous = true;
          break;
        }
      }
      if (need_clear_previous) {
        // Disposal 2 clears only the previous frame's rectangle: grow it to
        // cover every pixel that must become transparent. The added area is
        // drawn with the transparent index, so it shows what was beneath.
        EncodedFrame& prev = encoded.back();
        std::uint32_t x0 = prev.rect.x, y0 = prev.rect.y;
        std::uint32_t x1 = prev.rect.x + prev.rect.w - 1, y1 = prev.rect.y + prev.rect.h - 1;
        for (std::uint32_t y = 0; y < height; ++y) {
          for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            if (shown[i * 4 + 3] == 0 && screen[i * 4 + 3] != 0) {
              x0 = std::min(x0, x);
              x1 = std::max(x1, x);
              y0 = std::min(y0, y);
              y1 = std::max(y1, y);
            }
          }
        }
        if (prev.clear < 0) {  // no transparent entry: redraw the whole previous picture
          x0 = 0;
          y0 = 0;
          x1 = width - 1;
          y1 = height - 1;
        }
        const Rect grown{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        std::vector<std::uint8_t> grown_indices(static_cast<std::size_t>(grown.w) * grown.h,
                                                static_cast<std::uint8_t>(std::max(prev.clear, 0)));
        if (prev.clear < 0) grown_indices = previous_full;
        for (std::uint32_t y = 0; y < prev.rect.h; ++y) {
          ::cvt::util::copy_bytes(grown_indices.data() + static_cast<std::size_t>(prev.rect.y + y - grown.y) * grown.w +
                          (prev.rect.x - grown.x),
                      prev.indices.data() + static_cast<std::size_t>(y) * prev.rect.w, prev.rect.w);
        }
        prev.rect = grown;
        prev.indices = std::move(grown_indices);
        if (prev.clear >= 0) prev.transparent = prev.clear;
        prev.disposal = 2;
        for (std::uint32_t y = prev.rect.y; y < prev.rect.y + prev.rect.h; ++y) {
          ::cvt::util::fill_bytes(screen.data() + (static_cast<std::size_t>(y) * width + prev.rect.x) * 4, 0,
                      static_cast<std::size_t>(prev.rect.w) * 4);
        }
      }
      std::uint32_t x0 = width, y0 = height, x1 = 0, y1 = 0;
      for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
          const std::size_t i = static_cast<std::size_t>(y) * width + x;
          if (::cvt::util::compare_bytes(shown.data() + i * 4, screen.data() + i * 4, 4) != 0) {
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
          }
        }
      }
      rect = x0 > x1 ? Rect{0, 0, 1, 1} : Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }
    ef.rect = rect;
    ef.indices.resize(static_cast<std::size_t>(rect.w) * rect.h);
    for (std::uint32_t y = 0; y < rect.h; ++y) {
      for (std::uint32_t x = 0; x < rect.w; ++x) {
        const std::size_t i = static_cast<std::size_t>(rect.y + y) * width + rect.x + x;
        std::uint8_t index = q.indices[i];
        // Unchanged pixels become transparent: long runs compress far better.
        if (n > 0 && clear >= 0 && ::cvt::util::compare_bytes(shown.data() + i * 4, screen.data() + i * 4, 4) == 0) {
          index = static_cast<std::uint8_t>(clear);
        }
        ef.indices[static_cast<std::size_t>(y) * rect.w + x] = index;
      }
    }
    // Update the screen with what this frame draws.
    for (std::uint32_t y = 0; y < rect.h; ++y) {
      for (std::uint32_t x = 0; x < rect.w; ++x) {
        const std::size_t i = static_cast<std::size_t>(rect.y + y) * width + rect.x + x;
        const std::uint8_t index = ef.indices[static_cast<std::size_t>(y) * rect.w + x];
        if (static_cast<int>(index) == clear) continue;
        ::cvt::util::copy_bytes(screen.data() + i * 4, palette[index].data(), 4);
      }
    }
    if (ef.transparent >= 0) {
      // Keep the transparency flag only when the frame really uses it.
      if (std::find(ef.indices.begin(), ef.indices.end(), static_cast<std::uint8_t>(ef.transparent)) == ef.indices.end()) {
        ef.transparent = -1;
      }
    }
    encoded.push_back(std::move(ef));
    previous_full = std::move(q.indices);
  }

  util::ByteWriter out;
  out.text("GIF89a");
  out.u16le(static_cast<std::uint16_t>(width)).u16le(static_cast<std::uint16_t>(height));
  if (!use_local) {
    const int bits = bits_for(std::max<std::size_t>(global.size(), 2));
    out.u8(static_cast<std::uint8_t>(0x80 | ((bits - 1) << 4) | (bits - 1)));
  } else {
    out.u8(0);
  }
  out.u8(0).u8(0);
  if (!use_local) write_palette(out, global);
  if (animated && image.loop_count != 1) {
    out.u8(0x21).u8(0xFF).u8(11).text("NETSCAPE2.0").u8(3).u8(1);
    const std::uint32_t repeats = image.loop_count == 0 ? 0 : image.loop_count - 1;
    out.u16le(static_cast<std::uint16_t>(std::min<std::uint32_t>(repeats, 65535))).u8(0);
  }
  if (prepared.keep_metadata()) {
    if (const auto it = image.metadata.find("Comment"); it != image.metadata.end() && !it->second.empty()) {
      out.u8(0x21).u8(0xFE);
      const std::string& text = it->second;
      for (std::size_t at = 0; at < text.size(); at += 255) {
        const std::size_t n = std::min<std::size_t>(255, text.size() - at);
        out.u8(static_cast<std::uint8_t>(n)).text(std::string_view(text).substr(at, n));
      }
      out.u8(0);
    }
  }
  for (const EncodedFrame& ef : encoded) {
    if (animated || ef.transparent >= 0) {
      out.u8(0x21).u8(0xF9).u8(4);
      out.u8(static_cast<std::uint8_t>((animated ? ef.disposal : 0) << 2 | (ef.transparent >= 0 ? 1 : 0)));
      out.u16le(static_cast<std::uint16_t>(std::min<std::uint32_t>(ef.delay_cs, 65535)));
      out.u8(static_cast<std::uint8_t>(ef.transparent >= 0 ? ef.transparent : 0)).u8(0);
    }
    out.u8(0x2C);
    out.u16le(static_cast<std::uint16_t>(ef.rect.x)).u16le(static_cast<std::uint16_t>(ef.rect.y));
    out.u16le(static_cast<std::uint16_t>(ef.rect.w)).u16le(static_cast<std::uint16_t>(ef.rect.h));
    const std::vector<Rgba>& palette = ef.local_palette.empty() ? global : ef.local_palette;
    const int bits = bits_for(std::max<std::size_t>(palette.size(), 2));
    if (!ef.local_palette.empty()) {
      out.u8(static_cast<std::uint8_t>(0x80 | (bits - 1)));
      write_palette(out, ef.local_palette);
    } else {
      out.u8(0);
    }
    const int min_code = std::max(2, bits);
    out.u8(static_cast<std::uint8_t>(min_code));
    lzw_encode(ef.indices, min_code, out);
  }
  out.u8(0x3B);
  return out.take();
}

}  // namespace

void register_gif(Registry& registry) {
  FormatInfo info;
  info.id = "gif";
  info.name = "GIF image";
  info.description = "256-colour image or animation.";
  info.category = Category::Image;
  info.extensions = {"gif"};
  info.mime = "image/gif";
  info.popularity = 85;
  info.signatures = {Signature::at(0, "'GIF87a'"), Signature::at(0, "'GIF89a'")};
  registry.add_format(std::move(info));

  ImageCodecSpec spec;
  spec.format_id = "gif";
  spec.read = reader(read_gif);
  spec.write = writer(write_gif);
  spec.write_options = lossy_writer_options();
  spec.write_options.front().description =
      "GIF holds at most 256 colours. Pictures with more are reduced to a palette; lower quality uses fewer "
      "colours and makes smaller files.";
  spec.write_options.push_back(dither_option());
  spec.write_options.push_back(OptionSpec::choice("palette", "Palette", "auto", {"auto", "global", "local"},
                                                  "One palette for the whole animation, one per frame (better "
                                                  "colours, slightly larger), or automatic.")
                                   .advanced());
  spec.lossy_write = true;
  add_image_codec(registry, std::move(spec));
}

}  // namespace cvt::image
