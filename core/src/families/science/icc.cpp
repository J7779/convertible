// Convertible core - science family: ICC colour profiles (.icc/.icm, v2 and
// v4) summarised as Data: header fields and the common tags (description,
// copyright, white point, colorant matrix, tone curves, adaptation matrix);
// other tags are listed with their type and size.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>

#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

std::string four_cc(std::uint32_t v) {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = static_cast<char>((v >> shift) & 0xFF);
    out.push_back(c >= 32 && c < 127 ? c : '?');
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

double s15f16(util::ByteReader& r) { return static_cast<double>(r.i32be()) / 65536.0; }

Json tag_value(ByteView data) {
  util::ByteReader r(data);
  const std::string type = four_cc(r.u32be());
  (void)r.u32be();
  if (type == "desc") {
    const std::uint32_t n = r.u32be();
    std::string_view text = r.chars(std::min<std::size_t>(n, r.remaining()));
    text = text.substr(0, text.find('\0'));
    return Json(util::sanitize_utf8(text));
  }
  if (type == "text") {
    std::string_view text = as_chars(r.rest());
    return Json(util::sanitize_utf8(text.substr(0, text.find('\0'))));
  }
  if (type == "mluc") {
    const std::uint32_t records = r.u32be();
    const std::uint32_t size = r.u32be();
    if (records == 0 || size < 12) return Json("");
    (void)r.u16be();  // language
    (void)r.u16be();  // country
    const std::uint32_t length = r.u32be();
    const std::uint32_t offset = r.u32be();
    if (offset > data.size() || length > data.size() - offset) return Json("");
    util::ByteReader s(data.subspan(offset, length));
    std::u16string units;
    while (s.remaining() >= 2) units.push_back(s.u16be());
    while (!units.empty() && units.back() == 0) units.pop_back();
    return Json(util::utf16_to_utf8(units));
  }
  if (type == "XYZ") {
    Json list = Json::array();
    while (r.remaining() >= 12) {
      const double x = s15f16(r), y = s15f16(r), z = s15f16(r);
      list.push_back(Json::array({json_number(std::round(x * 1e6) / 1e6), json_number(std::round(y * 1e6) / 1e6),
                                  json_number(std::round(z * 1e6) / 1e6)}));
    }
    return list.size() == 1 ? Json(list[0]) : std::move(list);
  }
  if (type == "curv") {
    const std::uint32_t n = r.u32be();
    if (n == 0) return Json::object({{"type", "curv"}, {"gamma", 1.0}});
    if (n == 1) return Json::object({{"type", "curv"}, {"gamma", json_number(r.u16be() / 256.0)}});
    return Json::object({{"type", "curv"}, {"entries", n}});
  }
  if (type == "para") {
    const std::uint16_t function = r.u16be();
    (void)r.u16be();
    Json params = Json::array();
    while (r.remaining() >= 4 && params.size() < 7) params.push_back(json_number(std::round(s15f16(r) * 1e6) / 1e6));
    return Json::object({{"type", "para"}, {"function", function}, {"parameters", std::move(params)}});
  }
  if (type == "sf32") {
    Json list = Json::array();
    while (r.remaining() >= 4 && list.size() < 64) list.push_back(json_number(std::round(s15f16(r) * 1e6) / 1e6));
    return list;
  }
  if (type == "sig") return Json(four_cc(r.u32be()));
  return Json::object({{"type", type}, {"bytes", static_cast<std::uint64_t>(data.size())}});
}

Result<AnyIR> read_icc(ReadContext& ctx) {
  const ByteView in = ctx.input;
  if (in.size() < 132 || as_chars(in.subspan(36, 4)) != "acsp") return fail(Status::Decode, "not an ICC profile");
  util::ByteReader r(in);
  const std::uint32_t size = r.u32be();
  Json header = Json::object();
  header["size"] = size;
  header["cmm"] = four_cc(r.u32be());
  const std::uint32_t version = r.u32be();
  header["version"] = std::format("{}.{}.{}", version >> 24, (version >> 20) & 15, (version >> 16) & 15);
  header["device_class"] = four_cc(r.u32be());
  header["color_space"] = four_cc(r.u32be());
  header["pcs"] = four_cc(r.u32be());
  std::array<std::uint16_t, 6> date{};
  for (auto& d : date) d = r.u16be();
  header["created"] = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", date[0], date[1], date[2], date[3], date[4], date[5]);
  (void)r.u32be();  // acsp
  header["platform"] = four_cc(r.u32be());
  header["flags"] = r.u32be();
  header["manufacturer"] = four_cc(r.u32be());
  header["model"] = four_cc(r.u32be());
  (void)r.u64be();
  static constexpr std::string_view intents[] = {"perceptual", "relative colorimetric", "saturation", "absolute colorimetric"};
  const std::uint32_t intent = r.u32be();
  header["rendering_intent"] = intent < 4 ? std::string(intents[intent]) : std::to_string(intent);
  const double x = s15f16(r), y = s15f16(r), z = s15f16(r);
  header["illuminant"] = Json::array({json_number(std::round(x * 1e6) / 1e6), json_number(std::round(y * 1e6) / 1e6),
                                      json_number(std::round(z * 1e6) / 1e6)});
  header["creator"] = four_cc(r.u32be());
  (void)r.bytes(16);  // profile id
  util::ByteReader tags_reader(in.subspan(128));
  const std::uint32_t count = tags_reader.u32be();
  if (count > 4096 || std::size_t{count} * 12 > tags_reader.remaining()) return fail(Status::Decode, "bad ICC tag table");
  Json tags = Json::object();
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string sig = four_cc(tags_reader.u32be());
    const std::uint32_t offset = tags_reader.u32be();
    const std::uint32_t length = tags_reader.u32be();
    if (offset > in.size() || length > in.size() - offset || length < 8) {
      ctx.warn("ICC tag '" + sig + "' points outside the profile.");
      continue;
    }
    tags[sig] = tag_value(in.subspan(offset, length));
  }
  return ir::Data{Json::object({{"header", std::move(header)}, {"tags", std::move(tags)}})};
}

}  // namespace

void register_icc(Registry& registry) {
  add_format(registry, {.id = "icc",
                        .name = "ICC colour profile",
                        .description = "International Color Consortium device profile.",
                        .category = Category::Data,
                        .extensions = {"icc", "icm"},
                        .mime = "application/vnd.iccprofile",
                        .popularity = 15,
                        .signatures = {Signature::at(36, "'acsp'")}});
  add_codec(registry, "icc", Kind::Data, read_icc, nullptr);
}

}  // namespace cvt::science
