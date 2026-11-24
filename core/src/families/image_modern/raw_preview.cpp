// Convertible core - image_modern family: embedded previews of RAW files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/raw_model.hpp"

namespace cvt::image_modern::raw {

std::optional<std::array<std::uint32_t, 2>> jpeg_size(ByteView jpeg) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(jpeg.data());
  const std::size_t n = jpeg.size();
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return std::nullopt;
  std::size_t pos = 2;
  for (int guard = 0; guard < 512 && pos + 4 <= n; ++guard) {
    if (p[pos] != 0xFF) return std::nullopt;
    const std::uint8_t marker = p[pos + 1];
    if (marker == 0xFF) {
      ++pos;
      continue;
    }
    const std::size_t length = (std::size_t{p[pos + 2]} << 8) | p[pos + 3];
    if ((marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)) {
      if (pos + 9 > n) return std::nullopt;
      const std::uint32_t h = (std::uint32_t{p[pos + 5]} << 8) | p[pos + 6];
      const std::uint32_t w = (std::uint32_t{p[pos + 7]} << 8) | p[pos + 8];
      if (w == 0 || h == 0) return std::nullopt;
      return std::array<std::uint32_t, 2>{w, h};
    }
    if (marker == 0xDA || marker == 0xD9 || length < 2) return std::nullopt;
    pos += 2 + length;
  }
  return std::nullopt;
}

void add_jpeg_preview(ByteView jpeg, int orientation, std::vector<Preview>& out) {
  if (jpeg.size() < 64) return;
  const auto size = jpeg_size(jpeg);
  if (!size) return;
  // A lossless (SOF3) stream is RAW data, not a preview.
  const auto* p = reinterpret_cast<const std::uint8_t*>(jpeg.data());
  for (std::size_t i = 2; i + 1 < std::min<std::size_t>(jpeg.size(), 4096); ++i) {
    if (p[i] == 0xFF && p[i + 1] == 0xC3) return;
    if (p[i] == 0xFF && (p[i + 1] == 0xC0 || p[i + 1] == 0xC1 || p[i + 1] == 0xC2)) break;
  }
  for (const Preview& existing : out) {
    if (existing.data.data() == jpeg.data()) return;
  }
  Preview preview;
  preview.kind = Preview::Kind::Jpeg;
  preview.data = jpeg;
  preview.width = (*size)[0];
  preview.height = (*size)[1];
  preview.orientation = orientation;
  out.push_back(preview);
}

void collect_tiff_previews(const TiffTree& tree, int orientation, std::vector<Preview>& out) {
  const TiffData& t = tree.tiff;
  const auto add_ifd = [&](const Ifd& ifd) {
    // JPEGInterchangeFormat / Length.
    const std::uint64_t jpeg_at = t.integer(ifd, 513);
    const std::uint64_t jpeg_len = t.integer(ifd, 514);
    if (jpeg_at != 0 && jpeg_len != 0) add_jpeg_preview(t.range(jpeg_at, jpeg_len), orientation, out);
    const std::uint64_t compression = t.integer(ifd, 259, 1);
    const auto offsets = t.integers(ifd, 273, 2);
    const auto counts = t.integers(ifd, 279, 2);
    if (offsets.size() != 1 || counts.size() != 1) return;
    const ByteView strip = t.range(offsets[0], counts[0]);
    if (compression == 6 || compression == 7) {
      add_jpeg_preview(strip, orientation, out);
    } else if (compression == 1 && t.integer(ifd, 262) == 2 && t.integer(ifd, 277) == 3 && t.integer(ifd, 258) == 8) {
      const std::uint64_t w = t.integer(ifd, 256);
      const std::uint64_t h = t.integer(ifd, 257);
      if (w == 0 || h == 0 || w > 65535 || h > 65535 || strip.size() < w * h * 3) return;
      Preview preview;
      preview.kind = Preview::Kind::Rgb8;
      preview.data = strip;
      preview.width = static_cast<std::uint32_t>(w);
      preview.height = static_cast<std::uint32_t>(h);
      preview.orientation = orientation;
      out.push_back(preview);
    }
  };
  for (const Ifd& ifd : tree.ifds) {
    // CFA / linear RAW directories are not previews.
    const std::uint64_t photometric = t.integer(ifd, 262);
    if (photometric == 32803 || photometric == 34892) continue;
    add_ifd(ifd);
  }
}

Result<ir::Image> decode_largest_preview(const std::vector<Preview>& previews, const Context& ctx) {
  std::vector<const Preview*> order;
  for (const Preview& p : previews) order.push_back(&p);
  std::stable_sort(order.begin(), order.end(), [](const Preview* a, const Preview* b) {
    const std::uint64_t pa = std::uint64_t{a->width} * a->height;
    const std::uint64_t pb = std::uint64_t{b->width} * b->height;
    if (pa != pb) return pa > pb;
    return a->data.size() > b->data.size();
  });
  Error last{Status::Decode, "the RAW file contains no embedded preview"};
  for (const Preview* p : order) {
    if (p->kind == Preview::Kind::Rgb8) {
      if (auto checked = ctx.limits.check_pixels(p->width, p->height); !checked) return std::unexpected(checked.error());
      ir::Frame frame = ir::Frame::filled(p->width, p->height, {0, 0, 0, 255});
      const auto* s = reinterpret_cast<const std::uint8_t*>(p->data.data());
      for (std::size_t i = 0; i < std::size_t{p->width} * p->height; ++i) {
        frame.rgba[i * 4] = s[i * 3];
        frame.rgba[i * 4 + 1] = s[i * 3 + 1];
        frame.rgba[i * 4 + 2] = s[i * 3 + 2];
      }
      ir::Image image;
      image.frames.push_back(std::move(frame));
      if (p->orientation >= 2 && p->orientation <= 8) image::apply_orientation(image, p->orientation);
      return image;
    }
    auto decoded = image::decode("jpg", p->data, ctx);
    if (!decoded) {
      if (decoded.error().code == Status::Cancelled || decoded.error().code == Status::Limit) return decoded;
      last = decoded.error();
      continue;
    }
    ir::Image image = std::move(*decoded);
    // The JPEG reader applied the preview's own EXIF orientation; otherwise
    // the RAW file's orientation describes the preview too.
    if (image.exif.empty() && p->orientation >= 2 && p->orientation <= 8) image::apply_orientation(image, p->orientation);
    image.exif.clear();  // the preview's EXIF describes the camera, not this picture
    return image;
  }
  return std::unexpected(last);
}

}  // namespace cvt::image_modern::raw
