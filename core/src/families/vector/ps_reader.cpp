// Convertible core - vector family: reading PostScript, EPS and legacy Illustrator files.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <format>
#include <string>

#include "engine/standard_options.hpp"
#include "families/vector/ps_internal.hpp"
#include "families/vector/ps_io.hpp"
#include "util/text.hpp"

namespace cvt::vector {

namespace {

std::uint32_t le32(ByteView d, std::size_t at) {
  if (at + 4 > d.size()) return 0;
  return static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) |
         (static_cast<std::uint32_t>(d[at + 2]) << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24);
}

bool parse_box(std::string_view value, Rect& out) {
  double v[4];
  std::size_t pos = 0;
  for (double& x : v) {
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) ++pos;
    std::size_t end = pos;
    while (end < value.size() && value[end] != ' ' && value[end] != '\t' && value[end] != '\r' && value[end] != '\n') {
      ++end;
    }
    const auto parsed = util::parse_double(value.substr(pos, end - pos));
    if (!parsed || !std::isfinite(*parsed)) return false;
    x = *parsed;
    pos = end;
  }
  if (v[2] <= v[0] || v[3] <= v[1]) return false;
  out = Rect{v[0], v[1], v[2], v[3]};
  return true;
}

}  // namespace

ByteView postscript_section(ByteView input) {
  if (input.size() >= 30 && input[0] == std::byte{0xC5} && input[1] == std::byte{0xD0} &&
      input[2] == std::byte{0xD3} && input[3] == std::byte{0xC6}) {
    const std::uint32_t offset = le32(input, 4), length = le32(input, 8);
    if (offset >= input.size()) return {};
    return input.subspan(offset, std::min<std::size_t>(length, input.size() - offset));
  }
  return input;
}

DscInfo parse_dsc(std::string_view text) {
  DscInfo info;
  // Header comments (up to %%EndComments) and the trailer (for "(atend)").
  const auto scan = [&](std::string_view region, bool trailer) {
    std::size_t pos = 0;
    int lines = 0;
    while (pos < region.size() && lines < 20000) {
      std::size_t end = region.find_first_of("\r\n", pos);
      if (end == std::string_view::npos) end = region.size();
      const std::string_view line = region.substr(pos, end - pos);
      pos = end + 1;
      ++lines;
      if (!line.starts_with("%%")) {
        if (!trailer && !line.empty() && line[0] != '%' && lines > 1) break;
        continue;
      }
      if (!trailer && line.starts_with("%%EndComments")) break;
      const auto value = [&](std::string_view key) -> std::optional<std::string_view> {
        if (!line.starts_with(key)) return std::nullopt;
        return util::trim(line.substr(key.size()));
      };
      if (auto v = value("%%HiResBoundingBox:")) {
        Rect r;
        if (parse_box(*v, r)) info.hires = r;
      } else if (auto b = value("%%BoundingBox:")) {
        Rect r;
        if (parse_box(*b, r)) info.bbox = r;
        else if (b->find("atend") != std::string_view::npos) info.atend = true;
      } else if (auto t = value("%%Title:")) {
        if (!trailer) info.title = std::string(*t);
      } else if (auto p = value("%%Pages:")) {
        if (auto n = util::parse_int(util::trim(p->substr(0, p->find(' '))))) info.pages = static_cast<int>(*n);
      } else if (auto m = value("%%DocumentMedia:")) {
        // name width height weight colour type
        const auto parts = util::split(*m, ' ', false);
        if (parts.size() >= 3) {
          const auto w = util::parse_double(parts[1]), h = util::parse_double(parts[2]);
          if (w && h && *w > 0 && *h > 0) info.media = Rect{0, 0, *w, *h};
        }
      }
    }
  };
  scan(text.substr(0, std::min<std::size_t>(text.size(), 1u << 20)), false);
  if (info.atend || (info.bbox.empty() && text.size() > 4096)) {
    const std::size_t tail = text.size() > 65536 ? text.size() - 65536 : 0;
    const std::size_t trailer = text.rfind("%%Trailer");
    scan(text.substr(trailer != std::string_view::npos && trailer >= tail ? trailer : tail), true);
  }
  return info;
}

Result<Scene> read_postscript(ByteView input, ReadContext& ctx, PsKind kind) {
  ByteView section = postscript_section(input);
  if (section.empty()) return fail(Status::Decode, "the EPS preview header points outside the file");
  std::string_view text = as_chars(section);
  // Skip a leading ^D or a PJL / printer prefix before "%!".
  const std::size_t start = text.substr(0, std::min<std::size_t>(text.size(), 4096)).find("%!");
  if (text.starts_with("%PDF") || text.substr(0, std::min<std::size_t>(text.size(), 1024)).find("%PDF-") != std::string_view::npos) {
    if (kind == PsKind::AI) return fail(Status::Decode, "PDF-based Illustrator file; open it as PDF");
    return fail(Status::Decode, "this is a PDF file, not PostScript");
  }
  if (start != std::string_view::npos) text.remove_prefix(start);
  while (!text.empty() && (text.back() == '\x04' || text.back() == '\0')) text.remove_suffix(1);
  if (text.empty()) return fail(Status::Decode, "empty PostScript file");

  const DscInfo dsc = parse_dsc(text);
  ps::Options o;
  const bool epsf = text.substr(0, std::min<std::size_t>(text.size(), 200)).find("EPSF") != std::string_view::npos;
  o.eps = kind == PsKind::EPS || kind == PsKind::AI || (kind == PsKind::PS && epsf && dsc.pages <= 1);
  o.ai = kind == PsKind::AI || ps::needs_ai_emulation(text);
  o.max_operations = static_cast<std::uint64_t>(ctx.options.get_int("max_operations", 30'000'000, 1000, 2'000'000'000));
  o.max_vm_bytes = std::min<std::uint64_t>(512ull * 1024 * 1024, std::max<std::uint64_t>(ctx.limits.max_decompressed_bytes, 1u << 20));
  const Rect box = !dsc.hires.empty() ? dsc.hires : dsc.bbox;
  o.bbox = box;
  if (o.eps && box.empty()) {
    ctx.warn("the EPS file has no %%BoundingBox; using a Letter page");
    o.bbox = Rect{0, 0, 612, 792};
  }
  auto run = [&]() -> Result<Scene> {
    try {
      ps::Interpreter interp(ctx, o);
      if (!dsc.media.empty()) {
        interp.media_w_ = dsc.media.width();
        interp.media_h_ = dsc.media.height();
        interp.page_size_set_ = true;
      }
      return interp.run_program(text);
    } catch (const ps::FatalError& e) {
      return fail(e.error);
    } catch (const ps::PsError& e) {
      return fail(Status::Decode, "PostScript error during start-up: {}", e.name);
    } catch (const std::bad_alloc&) {
      return fail(Status::Limit, "the PostScript program needs too much memory");
    }
  };
  CVT_TRY_ASSIGN(Scene scene, run());
  if (!dsc.title.empty() && !options::resolved_strip_metadata(ctx.options)) {
    std::string title = dsc.title;
    if (title.size() >= 2 && title.front() == '(' && title.back() == ')') title = title.substr(1, title.size() - 2);
    scene.metadata["title"] = util::sanitize_utf8(title);
  }
  return scene;
}

}  // namespace cvt::vector
