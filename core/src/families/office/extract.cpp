// Convertible core - office family: the embedded pictures of a document as
// an Archive ("extract images": docx -> zip gives the pictures).
//
// Every Document reader of the family is offered a second, Archive-kind
// reader that runs it and collects the pictures in reading order. It costs
// more than the faithful routes, so it is used only for archive targets.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "engine/registry.hpp"
#include "families/document/document_api.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "ir/archive.hpp"
#include "util/hash.hpp"

namespace cvt::office {

namespace {

void collect(const ir::Blocks& blocks, std::vector<const ir::ImageSource*>& out, int depth);

void collect(const ir::Inlines& inlines, std::vector<const ir::ImageSource*>& out, int depth) {
  if (depth > ir::max_document_depth + 4) return;
  for (const ir::Inline& item : inlines) {
    if (const auto* image = item.get_if<ir::InlineImage>()) out.push_back(&image->source);
    std::visit(
        [&](const auto& node) {
          if constexpr (requires { node.children; }) collect(node.children, out, depth + 1);
        },
        item.node);
  }
}

void collect(const ir::Blocks& blocks, std::vector<const ir::ImageSource*>& out, int depth) {
  if (depth > ir::max_document_depth + 4) return;
  for (const ir::Block& b : blocks) {
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::ImageBlock>) {
            out.push_back(&node.source);
          } else if constexpr (std::same_as<T, ir::Paragraph> || std::same_as<T, ir::Heading>) {
            collect(node.content, out, depth + 1);
          } else if constexpr (std::same_as<T, ir::List>) {
            for (const ir::Blocks& item : node.items) collect(item, out, depth + 1);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            collect(node.blocks, out, depth + 1);
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            for (const ir::TableRow& row : node.rows) {
              for (const ir::TableCell& cell : row.cells) collect(cell.content, out, depth + 1);
            }
          }
        },
        b.node);
  }
}

ReadFn extracting(ReadFn reader) {
  return [reader = std::move(reader)](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(AnyIR value, reader(ctx));
    const ir::Document doc = std::move(value).get<ir::Document>();
    std::vector<const ir::ImageSource*> sources;
    collect(doc.blocks, sources, 0);
    for (const ir::Footnote& note : doc.footnotes) collect(note.blocks, sources, 0);
    ir::Archive archive;
    std::set<std::string> seen;
    for (const ir::ImageSource* source : sources) {
      auto image = document::encoded_image(*source);
      if (!image || image->bytes.empty()) continue;
      if (!seen.insert(util::sha1_hex(image->bytes)).second) continue;
      std::string number = std::to_string(archive.entries.size() + 1);
      while (number.size() < 3) number.insert(number.begin(), '0');
      std::string ext = extension_for_mime(image->mime.empty() ? document::sniff_image_mime(image->bytes) : image->mime);
      if (ext == "jpeg") ext = "jpg";
      ir::ArchiveEntry entry;
      entry.path = "image-" + number + "." + ext;
      entry.bytes = std::move(image->bytes);
      archive.entries.push_back(std::move(entry));
    }
    if (archive.entries.empty()) ctx.warn("The document contains no embedded pictures.");
    return AnyIR(std::move(archive));
  };
}

}  // namespace

void register_image_extraction(Registry& registry) {
  for (const char* id : {"docx", "docm", "dotx", "dotm", "wordml", "doc", "rtf", "odt", "ott", "fodt", "pptx", "pptm",
                         "ppsx", "potx", "ppt", "odp", "epub", "mobi", "azw3", "xps", "oxps", "abw"}) {
    ReadFn reader;
    for (const Codec* codec : registry.codecs_for(id)) {
      if (codec->kind == Kind::Document && codec->read) reader = codec->read;
    }
    if (!reader) continue;
    Codec codec;
    codec.format_id = id;
    codec.kind = Kind::Archive;
    codec.read = extracting(std::move(reader));
    codec.read_cost = 40;
    codec.lossy = true;
    registry.add_codec(std::move(codec));
  }
}

}  // namespace cvt::office
