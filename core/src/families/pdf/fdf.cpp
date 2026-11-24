// Convertible core - pdf family: FDF and XFDF form data (read as ir::Data).
//
// Both become {"file": <target PDF or null>, "fields": {"name": value, ...}}
// with nested field names joined by '.'; multi-valued fields become arrays.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <functional>

#include "engine/registry.hpp"
#include "families/pdf/document.hpp"
#include "families/pdf/pdf.hpp"
#include "families/pdf/syntax.hpp"
#include "ir/data.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::pdf {

namespace {

Json value_json(PdfFile& file, const Object& v_ref) {
  const Object v = file.resolve(v_ref);
  if (v.is_string()) return Json(util::sanitize_utf8(decode_text_string(v.as_string())));
  if (v.is_name()) return Json(v.as_name());
  if (v.is_bool()) return Json(v.as_bool());
  if (v.is_int()) return Json(v.as_int());
  if (v.is_number()) return Json(v.as_number());
  if (v.is_array()) {
    Json arr = Json::array();
    for (const Object& item : v.as_array()) arr.push_back(value_json(file, item));
    return arr;
  }
  if (v.is_stream()) return Json(util::sanitize_utf8(to_string(file.decode_or_empty(v))));
  return Json();
}

Result<AnyIR> read_fdf(ReadContext& ctx) {
  OpenOptions options;
  options.limits = ctx.limits;
  options.allow_fdf = true;
  CVT_TRY_ASSIGN(auto file, PdfFile::open(ctx.input, options));
  const Dict& fdf = file->get(file->catalog().as_dict(), "FDF").as_dict();
  if (fdf.empty()) return fail(Status::Decode, "the file has no FDF dictionary");
  Json fields = Json::object();
  std::size_t count = 0;
  std::function<void(const Object&, const std::string&, int)> walk = [&](const Object& field_ref, const std::string& prefix,
                                                                         int depth) {
    if (depth > 32 || ++count > 100000) return;
    const Dict& f = file->resolve(field_ref).as_dict();
    std::string name = util::sanitize_utf8(decode_text_string(file->get(f, "T").as_string()));
    const std::string full = prefix.empty() ? name : (name.empty() ? prefix : prefix + "." + name);
    if (f.contains("V")) fields.set(full, value_json(*file, f.get("V")));
    for (const Object& kid : file->get(f, "Kids").as_array()) walk(kid, full, depth + 1);
  };
  for (const Object& field : file->get(fdf, "Fields").as_array()) walk(field, "", 0);
  ir::Data data;
  data.value = Json::object();
  const Object target = file->get(fdf, "F");
  if (target.is_string()) data.value.set("file", Json(util::sanitize_utf8(decode_text_string(target.as_string()))));
  else if (target.is_dict()) data.value.set("file", Json(util::sanitize_utf8(decode_text_string(file->get(target.as_dict(), "F").as_string()))));
  else data.value.set("file", Json());
  data.value.set("fields", std::move(fields));
  return data;
}

Result<AnyIR> read_xfdf(ReadContext& ctx) {
  CVT_TRY_ASSIGN(util::XmlDocument xml, util::parse_xml(ctx.input));
  const util::XmlNode root = xml.root();
  if (root.local_name() != "xfdf") return fail(Status::Decode, "not an XFDF document");
  Json fields = Json::object();
  std::size_t count = 0;
  std::function<void(const util::XmlNode&, const std::string&, int)> walk = [&](const util::XmlNode& node,
                                                                                 const std::string& prefix, int depth) {
    if (depth > 32) return;
    for (const util::XmlNode field : node.elements()) {
      if (field.local_name() != "field" || ++count > 100000) continue;
      const std::string name(field.attribute("name").value());
      const std::string full = prefix.empty() ? name : prefix + "." + name;
      std::vector<std::string> values;
      for (const util::XmlNode v : field.elements()) {
        if (v.local_name() == "value" || v.local_name() == "value-richtext") values.push_back(v.text());
      }
      if (values.size() == 1) {
        fields.set(full, Json(values[0]));
      } else if (values.size() > 1) {
        Json arr = Json::array();
        for (std::string& v : values) arr.push_back(Json(std::move(v)));
        fields.set(full, std::move(arr));
      }
      walk(field, full, depth + 1);
    }
  };
  const util::XmlNode fields_node = root.child_local("fields");
  if (fields_node) walk(fields_node, "", 0);
  ir::Data data;
  data.value = Json::object();
  const util::XmlNode f = root.child_local("f");
  if (f) data.value.set("file", Json(std::string(f.attribute("href").value())));
  else data.value.set("file", Json());
  data.value.set("fields", std::move(fields));
  return data;
}

}  // namespace

void register_fdf(Registry& registry) {
  FormatInfo fdf;
  fdf.id = "fdf";
  fdf.name = "Forms Data Format";
  fdf.description = "Values of PDF form fields in PDF syntax.";
  fdf.category = Category::Data;
  fdf.extensions = {"fdf"};
  fdf.mime = "application/vnd.fdf";
  fdf.popularity = 8;
  fdf.signatures = {Signature::at(0, "'%FDF-'")};
  registry.add_format(std::move(fdf));
  Codec fdf_reader;
  fdf_reader.format_id = "fdf";
  fdf_reader.kind = Kind::Data;
  fdf_reader.read = read_fdf;
  registry.add_codec(std::move(fdf_reader));

  FormatInfo xfdf;
  xfdf.id = "xfdf";
  xfdf.name = "XML Forms Data Format";
  xfdf.description = "Values of PDF form fields in XML.";
  xfdf.category = Category::Data;
  xfdf.extensions = {"xfdf"};
  xfdf.mime = "application/vnd.adobe.xfdf";
  xfdf.popularity = 6;
  xfdf.sniff = [](std::span<const std::byte> data, std::string_view) -> int {
    const std::string_view head = as_chars(data.first(std::min<std::size_t>(data.size(), 512)));
    return head.find("<xfdf") != std::string_view::npos ? 92 : 0;
  };
  registry.add_format(std::move(xfdf));
  Codec xfdf_reader;
  xfdf_reader.format_id = "xfdf";
  xfdf_reader.kind = Kind::Data;
  xfdf_reader.read = read_xfdf;
  registry.add_codec(std::move(xfdf_reader));
}

}  // namespace cvt::pdf
