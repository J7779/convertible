// Helpers shared by the pdf family tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "families/pdf/document.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"

namespace cvt::pdf_test {

/// A gray reference render: zlib( u32le width, u32le height, pixels ).
inline ir::Frame load_gray_reference(std::string_view name) {
  const Bytes packed = test::fixture(std::string("pdf/ref/") + std::string(name) + ".p1.gray.zz");
  ir::Frame frame;
  auto raw = util::zlib_decompress(packed, 1u << 26);
  if (!raw || raw->size() < 8) return frame;
  util::ByteReader r(*raw);
  const std::uint32_t w = r.u32le(), h = r.u32le();
  if (static_cast<std::uint64_t>(w) * h + 8 > raw->size()) return frame;
  frame = ir::Frame::filled(w, h, {0, 0, 0, 255});
  for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
    const auto v = std::to_integer<std::uint8_t>((*raw)[8 + i]);
    frame.rgba[i * 4] = frame.rgba[i * 4 + 1] = frame.rgba[i * 4 + 2] = v;
  }
  return frame;
}

/// Luma copy of a frame composited on white (for comparison with gray references).
inline ir::Frame to_gray(const ir::Frame& f) {
  ir::Frame g = f;
  for (std::size_t i = 0; i < g.rgba.size(); i += 4) {
    const int a = f.rgba[i + 3];
    const auto blend = [&](int c) { return (c * a + 255 * (255 - a)) / 255; };
    const int v = (blend(f.rgba[i]) * 299 + blend(f.rgba[i + 1]) * 587 + blend(f.rgba[i + 2]) * 114 + 500) / 1000;
    g.rgba[i] = g.rgba[i + 1] = g.rgba[i + 2] = static_cast<std::uint8_t>(v);
    g.rgba[i + 3] = 255;
  }
  return g;
}

/// A binary PBM (P4) as 0/1 pixels (1 = black).
struct Pbm {
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> px;
};

inline Pbm load_pbm(std::string_view name) {
  const Bytes data = test::fixture(std::string("pdf/") + std::string(name));
  const std::string_view t = as_chars(data);
  Pbm pbm;
  if (!t.starts_with("P4")) return pbm;
  std::size_t p = 2;
  std::uint32_t values[2] = {0, 0};
  for (std::uint32_t& v : values) {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t' || t[p] == '#')) {
      if (t[p] == '#') {
        while (p < t.size() && t[p] != '\n') ++p;
      } else {
        ++p;
      }
    }
    while (p < t.size() && t[p] >= '0' && t[p] <= '9') v = v * 10 + static_cast<std::uint32_t>(t[p++] - '0');
  }
  ++p;
  pbm.width = values[0];
  pbm.height = values[1];
  const std::size_t row = (pbm.width + 7) / 8;
  pbm.px.assign(static_cast<std::size_t>(pbm.width) * pbm.height, 0);
  for (std::uint32_t y = 0; y < pbm.height; ++y) {
    for (std::uint32_t x = 0; x < pbm.width; ++x) {
      const std::size_t at = p + y * row + x / 8;
      if (at < t.size()) pbm.px[y * pbm.width + x] = (static_cast<std::uint8_t>(t[at]) >> (7 - x % 8)) & 1;
    }
  }
  return pbm;
}

/// Unpack filter output rows (MSB first) into 0/1 pixels, inverting when `one_is_white`.
inline std::vector<std::uint8_t> unpack(const Bytes& packed, std::uint32_t width, std::uint32_t height, bool one_is_white) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * height, 0);
  const std::size_t row = (width + 7) / 8;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t at = y * row + x / 8;
      if (at >= packed.size()) continue;
      int bit = (std::to_integer<int>(packed[at]) >> (7 - x % 8)) & 1;
      if (one_is_white) bit ^= 1;
      px[y * width + x] = static_cast<std::uint8_t>(bit);
    }
  }
  return px;
}

/// Plain text of a document read from a fixture.
inline std::string fixture_text(std::string_view name, std::string_view options = {}) {
  const Bytes data = test::fixture(std::string("pdf/") + std::string(name));
  auto doc = test::read_as<ir::Document>("pdf", data, options);
  if (!doc) return "ERROR: " + doc.error().message;
  return ir::plain_text(*doc);
}

}  // namespace cvt::pdf_test
