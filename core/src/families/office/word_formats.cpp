// Convertible core - office family: Word XML formats (DOCX family, flat OPC, Word 2003 XML).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <string>
#include <string_view>

#include "engine/registry.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

std::string lower_extension(std::string_view filename) {
  const std::size_t dot = filename.rfind('.');
  return dot == std::string_view::npos ? std::string() : util::to_lower(filename.substr(dot + 1));
}

// Which member of the DOCX family a package is. Content only for macros
// (the VBA project part is named in the directory); templates differ only
// in a content type inside a compressed part, so the file name decides.
std::string word_kind(ByteView data, std::string_view filename) {
  if (!zip_mentions(data, "word/document.xml") && !zip_mentions(data, "word/document2.xml")) return {};
  const std::string type = ooxml_main_content_type(data, "/word/document.xml");
  if (type.find("template.main") != std::string::npos) return "dotx";
  if (type.find("macroEnabledTemplate") != std::string::npos) return "dotm";
  if (type.find("macroEnabled") != std::string::npos) return "docm";
  if (!type.empty()) return "docx";
  // Truncated or unusual packages: the name, then a macro project.
  const std::string ext = lower_extension(filename);
  if (ext == "dotm" || ext == "dotx" || ext == "docm") return ext;
  return zip_mentions(data, "word/vbaProject.bin") ? "docm" : "docx";
}

int sniff_word_xml(ByteView data, std::string_view) {
  std::string_view head = as_chars(data).substr(0, 4096);
  if (head.starts_with("\xEF\xBB\xBF")) head.remove_prefix(3);
  if (!util::trim_left(head).starts_with("<")) return 0;
  if (head.find("progid=\"Word.Document\"") != std::string_view::npos) return 96;
  if (head.find("<w:wordDocument") != std::string_view::npos) return 94;
  if (head.find("<pkg:package") != std::string_view::npos &&
      as_chars(data).substr(0, 1 << 16).find("/word/document.xml") != std::string_view::npos) {
    return 94;
  }
  return 0;
}

void add_word_format(Registry& registry, std::string id, std::string name, std::string description,
                     std::string mime, int popularity, bool writer) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Document;
  info.extensions = {id};
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = [id](ByteView data, std::string_view filename) {
    const std::string kind = word_kind(data, filename);
    if (kind.empty()) return 0;
    if (kind == id) return 96;
    return id == "docx" ? 90 : 0;
  };
  registry.add_format(std::move(info));

  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Document;
  reader.read = docx_reader();
  reader.read_options = document_read_options();
  registry.add_codec(std::move(reader));
  if (writer) {
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Document;
    codec.write = docx_writer();
    codec.write_options = document_write_options(true);
    registry.add_codec(std::move(codec));
  }
}

}  // namespace

void register_word(Registry& registry) {
  add_word_format(registry, "docx", "Word document", "Microsoft Word document (Office Open XML).",
                  "application/vnd.openxmlformats-officedocument.wordprocessingml.document", 95, true);
  add_word_format(registry, "docm", "Word macro-enabled document",
                  "Word document with macros (the macros are never run or kept).",
                  "application/vnd.ms-word.document.macroEnabled.12", 30, false);
  add_word_format(registry, "dotx", "Word template", "Microsoft Word template (Office Open XML).",
                  "application/vnd.openxmlformats-officedocument.wordprocessingml.template", 30, false);
  add_word_format(registry, "dotm", "Word macro-enabled template",
                  "Word template with macros (the macros are never run or kept).",
                  "application/vnd.ms-word.template.macroEnabledTemplate", 15, false);

  FormatInfo xml;
  xml.id = "wordml";
  xml.name = "Word XML document";
  xml.description = "Word 2003 XML or flat Office Open XML: a whole Word document in one XML file.";
  xml.category = Category::Document;
  xml.extensions = {"xml"};
  xml.mime = "application/xml";
  xml.popularity = 15;
  xml.sniff = sniff_word_xml;
  registry.add_format(std::move(xml));

  Codec reader;
  reader.format_id = "wordml";
  reader.kind = Kind::Document;
  reader.read = flat_word_reader();
  reader.read_options = document_read_options();
  registry.add_codec(std::move(reader));
  Codec writer;
  writer.format_id = "wordml";
  writer.kind = Kind::Document;
  writer.write = word2003_writer();
  writer.write_options = document_write_options(true);
  registry.add_codec(std::move(writer));
}

}  // namespace cvt::office
