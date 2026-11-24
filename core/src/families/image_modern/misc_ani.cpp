// Convertible core - image_modern family: Windows animated cursors (.ani)
// and multi-page PCX (.dcx).
//
// ANI: a RIFF "ACON" form with an `anih` header, optional `rate` (per-step
// display time in 1/60 s) and `seq ` (step -> frame index) chunks and a
// `LIST fram` of `icon` chunks, each a complete ICO/CUR file decoded by the
// image family. Every step becomes a frame with its delay.
// DCX: a table of up to 1023 page offsets followed by PCX pages, each
// decoded / encoded by the image family's PCX codec.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "families/image/common.hpp"
#include "families/image/image_api.hpp"
#include "families/image_modern/misc.hpp"
#include "util/bytes.hpp"

namespace cvt::image_modern {

namespace {

using image::ImageCodecSpec;

// --------------------------------------------------------------------------- ANI

constexpr std::uint32_t ani_flag_icon = 1;
constexpr std::uint32_t ani_flag_sequence = 2;
constexpr std::size_t max_ani_steps = 65536;

struct AniChunks {
  bool have_header = false;
  std::uint32_t frames = 0;
  std::uint32_t steps = 0;
  std::uint32_t rate = 0;  // jiffies (1/60 s)
  std::uint32_t flags = 0;
  std::vector<std::uint32_t> rates;
  std::vector<std::uint32_t> sequence;
  std::vector<ByteView> icons;
  std::string title;
};

std::vector<std::uint32_t> read_dwords(ByteView data, std::size_t limit) {
  std::vector<std::uint32_t> values;
  util::ByteReader r(data);
  while (r.remaining() >= 4 && values.size() < limit) values.push_back(r.u32le());
  return values;
}

std::string info_text(ByteView data) {
  std::string text(as_chars(data));
  const std::size_t nul = text.find('\0');
  if (nul != std::string::npos) text.resize(nul);
  return text;
}

/// Walk the chunks of a RIFF list body.
Result<void> parse_ani_chunks(ByteView body, AniChunks& out, const Context& ctx, int depth) {
  if (depth > 4) return fail(Status::Decode, "ANI lists are nested too deeply");
  util::ByteReader r(body);
  while (r.remaining() >= 8) {
    const std::string id(r.chars(4));
    const std::uint32_t size = r.u32le();
    if (size > r.remaining()) {
      ctx.warn("The ANI file is truncated.");
      break;
    }
    const ByteView data = r.bytes(size);
    if (size % 2 != 0 && r.remaining() > 0) (void)r.skip(1);
    if (id == "anih") {
      if (size < 36) return fail(Status::Decode, "ANI header chunk is too short");
      util::ByteReader h(data);
      (void)h.u32le();  // cbSize
      out.frames = h.u32le();
      out.steps = h.u32le();
      (void)h.skip(16);  // width, height, bit count, planes (unused for icon frames)
      out.rate = h.u32le();
      out.flags = h.u32le();
      out.have_header = true;
    } else if (id == "rate") {
      out.rates = read_dwords(data, max_ani_steps);
    } else if (id == "seq ") {
      out.sequence = read_dwords(data, max_ani_steps);
    } else if (id == "LIST" && size >= 4) {
      const std::string_view type = as_chars(data.first(4));
      if (type == "fram" || type == "INFO") CVT_TRY(parse_ani_chunks(data.subspan(4), out, ctx, depth + 1));
    } else if (id == "icon") {
      CVT_TRY(ctx.limits.check_archive_entries(out.icons.size() + 1));
      out.icons.push_back(data);
    } else if (id == "INAM") {
      out.title = info_text(data);
    }
  }
  return {};
}

Result<ir::Image> read_ani(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (!r.consume("RIFF")) return fail(Status::Decode, "not a RIFF file");
  const std::uint32_t riff_size = r.u32le();
  if (!r.consume("ACON")) return fail(Status::Decode, "not an animated cursor (RIFF ACON)");
  const std::size_t body = std::min<std::size_t>(r.remaining(), riff_size >= 4 ? riff_size - 4 : 0);
  AniChunks chunks;
  CVT_TRY(parse_ani_chunks(r.bytes(body), chunks, ctx, 0));
  if (!chunks.have_header) return fail(Status::Decode, "ANI file has no 'anih' header");
  if ((chunks.flags & ani_flag_icon) == 0) {
    return fail(Status::UnsupportedConversion, "ANI frames stored as raw bitmaps are not supported");
  }
  if (chunks.icons.empty()) return fail(Status::Decode, "ANI file has no frames");

  // Decode each distinct frame once.
  std::vector<ir::Frame> decoded(chunks.icons.size());
  std::vector<bool> ok(chunks.icons.size(), false);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  for (std::size_t i = 0; i < chunks.icons.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(chunks.icons.size())));
    const ByteView icon = chunks.icons[i];
    // Byte 2 of an ICONDIR is the type: 1 = icon, 2 = cursor.
    const bool cursor = icon.size() > 2 && std::to_integer<int>(icon[2]) == 2;
    auto picture = image::decode(cursor ? "cur" : "ico", icon, ctx);
    if (!picture || !picture->valid()) {
      ctx.warn("An ANI frame could not be decoded and was skipped.");
      continue;
    }
    decoded[i] = std::move(picture->frames.front());
    ok[i] = true;
    if (width == 0) {
      width = decoded[i].width;
      height = decoded[i].height;
    }
  }
  if (width == 0) return fail(Status::Decode, "no ANI frame could be decoded");

  // Steps: the sequence when present, else the frames in order.
  std::vector<std::uint32_t> steps;
  if ((chunks.flags & ani_flag_sequence) != 0 && !chunks.sequence.empty()) {
    steps = chunks.sequence;
  } else {
    for (std::uint32_t i = 0; i < chunks.icons.size(); ++i) steps.push_back(i);
  }
  if (chunks.steps != 0 && steps.size() > chunks.steps) steps.resize(chunks.steps);

  ir::Image image;
  bool size_warned = false;
  for (std::size_t s = 0; s < steps.size(); ++s) {
    const std::uint32_t index = steps[s];
    if (index >= decoded.size() || !ok[index]) continue;
    const ir::Frame& source = decoded[index];
    if (source.width != width || source.height != height) {
      if (!size_warned) ctx.warn("ANI frames of a different size than the first were skipped.");
      size_warned = true;
      continue;
    }
    CVT_TRY(image::check_size(ctx, width, height, image.frames.size() + 1));
    ir::Frame frame = source;
    const std::uint32_t jiffies = s < chunks.rates.size() ? chunks.rates[s] : chunks.rate;
    frame.delay_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>((std::uint64_t{jiffies} * 1000 + 30) / 60, 0xFFFFFFFFu));
    image.frames.push_back(std::move(frame));
  }
  if (image.frames.empty()) return fail(Status::Decode, "the ANI sequence refers to no decodable frame");
  if (image.frames.size() == 1) image.frames[0].delay_ms = 0;
  if (!chunks.title.empty()) image.metadata["Title"] = chunks.title;
  return image;
}

// --------------------------------------------------------------------------- DCX

constexpr std::uint32_t dcx_magic = 0x3ADE68B1;
constexpr std::size_t dcx_max_pages = 1023;

Result<ir::Image> read_dcx(ReadContext& ctx) {
  util::ByteReader r(ctx.input);
  if (r.u32le() != dcx_magic || !r.ok()) return fail(Status::Decode, "not a DCX file");
  std::vector<std::uint32_t> offsets;
  while (offsets.size() < dcx_max_pages) {
    const std::uint32_t offset = r.u32le();
    if (!r.ok() || offset == 0) break;
    offsets.push_back(offset);
  }
  if (offsets.empty()) return fail(Status::Decode, "DCX file lists no pages");
  ir::Image image;
  bool size_warned = false;
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(offsets.size())));
    const std::uint64_t start = offsets[i];
    // A page ends where the next one in file order starts.
    std::uint64_t end = ctx.input.size();
    for (const std::uint32_t other : offsets) {
      if (other > start && other < end) end = other;
    }
    if (start >= ctx.input.size() || start < 8) {
      ctx.warn("A DCX page offset points outside the file; the page was skipped.");
      continue;
    }
    const ByteView page = ctx.input.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
    auto decoded = image::decode("pcx", page, ctx);
    if (!decoded || !decoded->valid()) {
      if (!decoded && decoded.error().code == Status::Cancelled) return fail(decoded.error());
      ctx.warn("A DCX page could not be decoded and was skipped.");
      continue;
    }
    ir::Frame frame = std::move(decoded->frames.front());
    if (!image.frames.empty() && (frame.width != image.width() || frame.height != image.height())) {
      if (!size_warned) ctx.warn("DCX pages of a different size than the first were skipped.");
      size_warned = true;
      continue;
    }
    CVT_TRY(image::check_size(ctx, frame.width, frame.height, image.frames.size() + 1));
    if (image.frames.empty()) {
      image.dpi_x = decoded->dpi_x;
      image.dpi_y = decoded->dpi_y;
    }
    frame.delay_ms = 0;
    image.frames.push_back(std::move(frame));
  }
  if (image.frames.empty()) return fail(Status::Decode, "no DCX page could be decoded");
  return image;
}

Result<Bytes> write_dcx(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const image::WriteImage prepared, image::prepare_write(source, ctx));
  const std::size_t pages = std::min(prepared->frames.size(), dcx_max_pages);
  if (prepared->frames.size() > dcx_max_pages) ctx.warn("DCX holds at most 1023 pages; the rest were dropped.");
  std::vector<Bytes> encoded;
  Options pcx_options;
  for (std::size_t i = 0; i < pages; ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(pages)));
    ir::Image page;
    page.frames.push_back(prepared->frames[i]);
    page.frames.back().delay_ms = 0;
    page.dpi_x = prepared->dpi_x;
    page.dpi_y = prepared->dpi_y;
    CVT_TRY_ASSIGN(Bytes pcx, image::encode("pcx", page, pcx_options, ctx));
    encoded.push_back(std::move(pcx));
  }
  util::ByteWriter out;
  out.u32le(dcx_magic);
  std::uint64_t offset = 4 + 4 * (pages + 1);
  for (const Bytes& page : encoded) {
    if (offset > 0xFFFFFFFFu) return fail(Status::Limit, "the DCX file would exceed 4 GiB");
    out.u32le(static_cast<std::uint32_t>(offset));
    offset += page.size();
  }
  out.u32le(0);
  for (const Bytes& page : encoded) out.bytes(page);
  return out.take();
}

}  // namespace

void register_misc_ani(Registry& registry) {
  FormatInfo ani;
  ani.id = "ani";
  ani.name = "Windows animated cursor";
  ani.description = "Animated mouse pointer: a sequence of cursor or icon frames.";
  ani.category = Category::Image;
  ani.extensions = {"ani"};
  ani.mime = "application/x-navi-animation";
  ani.popularity = 12;
  ani.signatures = {Signature::at(0, "'RIFF' ?? ?? ?? ?? 'ACON'")};
  registry.add_format(std::move(ani));

  FormatInfo dcx;
  dcx.id = "dcx";
  dcx.name = "Multi-page PCX (DCX)";
  dcx.description = "ZSoft multi-page PCX, used by fax software.";
  dcx.category = Category::Image;
  dcx.extensions = {"dcx"};
  dcx.mime = "image/x-dcx";
  dcx.popularity = 5;
  dcx.signatures = {Signature::at(0, "B1 68 DE 3A")};
  registry.add_format(std::move(dcx));

  ImageCodecSpec ani_codec;
  ani_codec.format_id = "ani";
  ani_codec.read = image::reader(read_ani);
  image::add_image_codec(registry, std::move(ani_codec));

  ImageCodecSpec dcx_codec;
  dcx_codec.format_id = "dcx";
  dcx_codec.read = image::reader(read_dcx);
  dcx_codec.write = image::writer(write_dcx);
  dcx_codec.write_options = image::plain_writer_options();
  image::add_image_codec(registry, std::move(dcx_codec));
}

}  // namespace cvt::image_modern
