// Convertible core - data family: generic XML <-> data mapping.
//
// Conventions (the widely used "xmltodict" style; every name is an option):
//   <root a="1"><b>x</b><b>y</b><c/></root>
//     -> {"root": {"@a": "1", "b": ["x", "y"], "c": null}}
//   * the document element becomes the single top-level key;
//   * attributes become members prefixed with `attribute_prefix` ("@"),
//     namespace declarations included ("@xmlns:x"), so qualified names and
//     namespaces survive unchanged;
//   * an element with only text becomes that string; text next to attributes
//     or child elements goes under `text_key` ("#text") - with mixed content
//     the text pieces are joined, so their position among the children is lost;
//   * repeated child elements become an array (`force_arrays` makes every
//     child element an array);
//   * an empty element is null; comments and processing instructions are
//     dropped; CDATA is text.
// Writing reverses the mapping. Arrays nested directly in arrays, and the
// elements of a top-level array, are written as `item_name` ("item")
// elements. Keys that are not XML names are made legal ('_' for invalid
// characters) with a warning.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::data {

namespace {

struct XmlMapping {
  std::string attribute_prefix = "@";
  std::string text_key = "#text";
  std::string item_name = "item";
  bool force_arrays = false;
  bool infer_types = false;
  bool strip = true;
};

XmlMapping mapping_from(const Options& options) {
  XmlMapping mapping;
  mapping.attribute_prefix = options.get_string("attribute_prefix", "@");
  mapping.text_key = options.get_string("text_key", "#text");
  mapping.item_name = options.get_string("item_name", "item");
  mapping.force_arrays = options.get_bool("force_arrays", false);
  mapping.infer_types = options.get_bool("infer_types", false);
  mapping.strip = options.get_bool("strip_whitespace", true);
  if (mapping.item_name.empty() || !util::xml_is_name(mapping.item_name)) mapping.item_name = "item";
  return mapping;
}

Json typed(std::string text, const XmlMapping& mapping) {
  if (!mapping.infer_types) return Json(std::move(text));
  if (text == "true") return Json(true);
  if (text == "false") return Json(false);
  // A JSON number literal, nothing else ("007" and "1e5x" stay strings).
  std::size_t i = 0;
  if (i < text.size() && text[i] == '-') ++i;
  const std::size_t digits = i;
  while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
  if (i == digits || (text[digits] == '0' && i - digits > 1)) return Json(std::move(text));
  if (i < text.size() && text[i] == '.') {
    const std::size_t frac = ++i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    if (i == frac) return Json(std::move(text));
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    const std::size_t exp = i;
    while (i < text.size() && util::is_ascii_digit(text[i])) ++i;
    if (i == exp) return Json(std::move(text));
  }
  if (i != text.size()) return Json(std::move(text));
  return number_from_literal(text);
}

Result<Json> element_to_json(util::XmlNode element, const XmlMapping& mapping, std::size_t depth, std::size_t max_depth) {
  if (depth > max_depth) {
    return fail(Status::Limit, "the XML data is nested more than {} levels deep (option max_depth)", max_depth);
  }
  Json object = Json::object();
  for (const util::XmlAttribute attribute : element.attributes()) {
    object.set(mapping.attribute_prefix + std::string(attribute.name()), typed(std::string(attribute.value()), mapping));
  }
  std::string text;
  bool has_children = false;
  for (const util::XmlNode child : element.children()) {
    if (child.is_text()) {
      text += child.value();
      continue;
    }
    if (!child.is_element()) continue;
    has_children = true;
    CVT_TRY_ASSIGN(Json value, element_to_json(child, mapping, depth + 1, max_depth));
    const std::string name(child.name());
    if (Json* existing = object.find(name)) {
      if (!existing->is_array()) {
        Json first = std::move(*existing);
        *existing = Json::array();
        existing->push_back(std::move(first));
      }
      existing->push_back(std::move(value));
    } else if (mapping.force_arrays) {
      object.set(name, Json::array({std::move(value)}));
    } else {
      object.set(name, std::move(value));
    }
  }
  if (mapping.strip) text = std::string(util::trim(text));
  if (object.empty()) {
    if (text.empty() && !has_children) return Json();
    return typed(std::move(text), mapping);
  }
  if (!text.empty()) object.set(mapping.text_key, typed(std::move(text), mapping));
  return object;
}

Result<Json> parse_xml_data(std::string_view text, ReadContext& ctx) {
  const ReadPolicy policy = read_policy(ctx.options);
  util::XmlParseOptions options;
  options.max_depth = policy.max_depth + 1;
  options.preserve_whitespace = !ctx.options.get_bool("strip_whitespace", true);
  auto document = util::parse_xml(text, options);
  if (!document) return fail(std::move(document).error());
  const util::XmlNode root = document->root();
  if (!root) return fail(Status::Decode, "the XML document has no root element");
  const XmlMapping mapping = mapping_from(ctx.options);
  CVT_TRY_ASSIGN(Json value, element_to_json(root, mapping, 0, policy.max_depth));
  Json out = Json::object();
  out.set(root.name(), std::move(value));
  return out;
}

// =========================================================================== writer

class XmlBuilder {
 public:
  explicit XmlBuilder(const XmlMapping& mapping) : mapping_(mapping) {}

  std::string name_for(std::string_view key) {
    if (util::xml_is_name(key)) return std::string(key);
    std::string name;
    std::size_t i = 0;
    while (i < key.size()) {
      const util::Utf8Decoded d = util::decode_utf8(key, i);
      std::string piece(key.substr(i, d.length));
      if (!d.ok || !util::xml_is_name("a" + piece)) piece = "_";
      name += piece;
      i += d.length;
    }
    if (name.empty() || !util::xml_is_name(name)) name.insert(0, "_");
    if (!util::xml_is_name(name)) name = "_";
    ++renamed_;
    return name;
  }

  static std::string scalar_text(const Json& value) {
    switch (tag_kind(value)) {
      case TagKind::None: break;
      case TagKind::Float: return scalar_text(plain_value(value));
      default: {
        const Json plain = plain_value(value);
        if (plain.is_string()) return plain.as_string();
        if (!plain.is_structured()) return scalar_text(plain);
        return util::dump_json(plain);
      }
    }
    switch (value.type()) {
      case Json::Type::Null: return {};
      case Json::Type::Bool: return value.as_bool() ? "true" : "false";
      case Json::Type::Int64:
      case Json::Type::UInt64: return format_integer(value);
      case Json::Type::Double: {
        const double d = value.as_double();
        return std::isfinite(d) ? format_double(d) : (std::isnan(d) ? "NaN" : d < 0 ? "-INF" : "INF");
      }
      case Json::Type::String: return value.as_string();
      default: return util::dump_json(value);
    }
  }

  [[nodiscard]] static bool is_leaf(const Json& value) {
    return !value.is_structured() || tag_kind(value) != TagKind::None;
  }

  /// Fill `element` from `value`.
  void fill(util::XmlNode element, const Json& value) {
    if (is_leaf(value)) {
      const std::string text = scalar_text(value);
      if (!text.empty()) element.append_text(text);
      return;
    }
    if (value.is_array()) {
      for (const Json& item : value.elements()) add_child(element, mapping_.item_name, item);
      return;
    }
    // Attributes first (XML puts them on the start tag anyway).
    for (const auto& member : value.items()) {
      const std::string& key = member.key();
      if (!mapping_.attribute_prefix.empty() && key.starts_with(mapping_.attribute_prefix) && is_leaf(member.value())) {
        const std::string name = name_for(std::string_view(key).substr(mapping_.attribute_prefix.size()));
        element.set_attribute(name, scalar_text(member.value()));
      }
    }
    for (const auto& member : value.items()) {
      const std::string& key = member.key();
      if (!mapping_.attribute_prefix.empty() && key.starts_with(mapping_.attribute_prefix) && is_leaf(member.value())) {
        continue;
      }
      if (key == mapping_.text_key && is_leaf(member.value())) {
        const std::string text = scalar_text(member.value());
        if (!text.empty()) element.append_text(text);
        continue;
      }
      const std::string name = name_for(key);
      if (member.value().is_array()) {
        for (const Json& item : member.value().elements()) add_child(element, name, item);
      } else {
        add_child(element, name, member.value());
      }
    }
  }

  void add_child(util::XmlNode parent, const std::string& name, const Json& value) {
    util::XmlNode child = parent.append_child(name);
    if (value.is_array()) {
      for (const Json& item : value.elements()) add_child(child, mapping_.item_name, item);
      return;
    }
    fill(child, value);
  }

  std::size_t renamed_ = 0;

 private:
  const XmlMapping& mapping_;
};

Result<std::string> write_xml_data(const Json& value, WriteContext& ctx) {
  const XmlMapping mapping = mapping_from(ctx.options);
  XmlBuilder builder(mapping);
  util::XmlDocument document;
  document.set_declaration("1.0", "UTF-8");
  const std::string root_name = ctx.options.get_string("root_name", "root");
  const bool single_root = value.is_object() && tag_kind(value) == TagKind::None && value.size() == 1 &&
                           !value.items()[0].value().is_array() && !value.items()[0].key().starts_with(mapping.attribute_prefix) &&
                           value.items()[0].key() != mapping.text_key;
  if (single_root) {
    const auto& member = value.items()[0];
    util::XmlNode root = document.append_child(builder.name_for(member.key()));
    builder.fill(root, member.value());
  } else {
    util::XmlNode root = document.append_child(util::xml_is_name(root_name) ? root_name : std::string("root"));
    builder.fill(root, value);
  }
  if (builder.renamed_ > 0) {
    ctx.warn(std::format("{} key(s) were not valid XML names and were adjusted.", builder.renamed_));
  }
  util::XmlWriteOptions options;
  const bool minify = same_format(ctx) && !ctx.options.was_set("indent");
  const auto indent = minify ? 0 : ctx.options.get_int("indent", 2, 0, 16);
  options.indent = std::string(static_cast<std::size_t>(indent), ' ');
  std::string out = util::xml_to_string(document, options);
  if (indent == 0 && !out.ends_with('\n')) out.push_back('\n');
  return out;
}

int sniff_xml_data(ByteView data, std::string_view filename) {
  const std::string_view head = text_head(data, 1024);
  if (head.starts_with("<?xml")) return 70;
  if (head.starts_with("<") && has_extension(filename, {"xml"})) return 72;
  if (has_extension(filename, {"xml"})) {
    // UTF-16 XML starts with "<" and NUL bytes.
    const std::string_view raw = as_chars(data);
    if (raw.size() >= 4 && ((raw[0] == '<' && raw[1] == '\0') || (raw[0] == '\0' && raw[1] == '<') ||
                            raw.starts_with("\xFF\xFE") || raw.starts_with("\xFE\xFF"))) {
      return 72;
    }
  }
  return 0;
}

Result<AnyIR> read_xml_codec(ReadContext& ctx) {
  // The XML parser does its own encoding detection (BOM, declaration).
  CVT_TRY_ASSIGN(Json value, parse_xml_data(as_chars(ctx.input), ctx));
  CVT_TRY(finish_read(value, ctx));
  return AnyIR(ir::Data{std::move(value)});
}

}  // namespace

void register_xml_data(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "xml",
      .name = "XML",
      .description = "Extensible Markup Language document.",
      .category = Category::Data,
      .extensions = {"xml"},
      .mime = "application/xml",
      .popularity = 80,
      .signatures = {},
      .sniff = sniff_xml_data,
  });
  std::vector<OptionSpec> mapping_options = {
      OptionSpec::text("attribute_prefix", "Attribute prefix", "@", "Prefix that marks attributes among the keys.")
          .advanced(),
      OptionSpec::text("text_key", "Text key", "#text", "Key holding the text of an element that also has "
                                                         "attributes or children.")
          .advanced(),
      OptionSpec::text("item_name", "Item element name", "item", "Element name used for the entries of nested lists.")
          .advanced(),
  };
  std::vector<OptionSpec> read_options = common_read_options(false, false);
  read_options.insert(read_options.end(), mapping_options.begin(), mapping_options.end());
  read_options.push_back(OptionSpec::boolean("force_arrays", "Always use arrays", false,
                                             "Make every child element an array, even when it occurs once."));
  read_options.push_back(OptionSpec::boolean("infer_types", "Detect numbers and booleans", false,
                                             "Turn text such as 42, 1.5 and true into numbers and booleans."));
  read_options.push_back(OptionSpec::boolean("strip_whitespace", "Trim text", true,
                                             "Remove white space around element text.")
                             .advanced());
  std::vector<OptionSpec> write_options = {
      indent_option(2),
      OptionSpec::text("root_name", "Root element", "root",
                       "Name of the document element when the data does not have exactly one top-level key."),
  };
  write_options.insert(write_options.end(), mapping_options.begin(), mapping_options.end());
  add_codec(registry, {.format_id = "xml",
                       .read = read_xml_codec,
                       .write = text_writer(write_xml_data, "XML"),
                       .read_options = std::move(read_options),
                       .write_options = std::move(write_options),
                       .lossy = true,
                       .read_cost = 15,
                       .write_cost = 15});
}

}  // namespace cvt::data
