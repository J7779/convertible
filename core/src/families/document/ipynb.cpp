// Convertible core - document family: Jupyter notebooks (nbformat 4, tolerant of 3).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "util/base64.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

// "source" is a string or an array of strings.
std::string joined(const Json& value) {
  if (value.is_string()) return value.as_string();
  std::string out;
  for (const Json& part : value.elements()) out += part.as_string();
  return out;
}

std::string strip_ansi(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1B' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) ++i;
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

std::optional<ir::EncodedImage> decode_image(const Json& value, std::string_view mime, const Context& ctx) {
  std::string data = joined(value);
  if (mime == "image/svg+xml") {
    ir::EncodedImage svg;
    svg.bytes = to_bytes(data);
    svg.mime = "image/svg+xml";
    return svg;
  }
  std::erase_if(data, [](char c) { return util::is_ascii_space(c); });
  if (!ctx.limits.check_decompressed(data.size())) return std::nullopt;
  auto bytes = util::base64_decode(data);
  if (!bytes) return std::nullopt;
  ir::EncodedImage image;
  image.bytes = std::move(*bytes);
  image.mime = std::string(mime);
  return image;
}

void add_markdown(std::string_view text, const Json& attachments, ir::Document& document, const Context& ctx) {
  MarkdownReadOptions options;
  options.resolve = [&](std::string_view url) -> std::optional<ir::EncodedImage> {
    if (!url.starts_with("attachment:")) return std::nullopt;
    const Json& entry = attachments[url.substr(11)];
    for (const auto& member : entry.items()) {
      if (member.key().starts_with("image/")) return decode_image(member.value(), member.key(), ctx);
    }
    return std::nullopt;
  };
  auto parsed = parse_markdown(text, options);
  if (!parsed) return;
  if (document.metadata.title.empty() && !parsed->metadata.title.empty()) document.metadata.title = parsed->metadata.title;
  // Raw HTML in cells (figures, images) is parsed so pictures survive.
  HtmlReadOptions html;
  html.resolve = options.resolve;
  html.read_metadata = false;
  const std::string base = std::to_string(document.footnotes.size());
  for (ir::Block& block : parsed->blocks) {
    if (auto* raw = block.get_if<ir::CodeBlock>(); raw != nullptr && raw->raw_format == "html") {
      if (auto converted = parse_html_text(raw->code, html)) {
        for (ir::Block& b : converted->blocks) document.blocks.push_back(std::move(b));
        continue;
      }
    }
    document.blocks.push_back(std::move(block));
  }
  // Footnote labels are made unique across cells.
  for (ir::Footnote& note : parsed->footnotes) {
    const std::string old = note.label;
    note.label = base + "-" + old;
    document.footnotes.push_back(std::move(note));
  }
}

Result<ir::Document> read_ipynb(std::string_view text, ReadContext& ctx) {
  util::JsonParseOptions options;
  options.replace_invalid_utf8 = true;
  auto json = util::parse_json(text, options);
  if (!json || !json->is_object()) return fail(Status::Decode, "not a Jupyter notebook (invalid JSON)");
  const Json& nb = *json;
  if (!nb.contains("cells") && !nb.contains("worksheets")) return fail(Status::Decode, "not a Jupyter notebook (no cells)");
  ir::Document document;
  std::string language = nb["metadata"]["kernelspec"]["language"].as_string();
  if (language.empty()) language = nb["metadata"]["language_info"]["name"].as_string();
  if (language.empty()) language = nb["metadata"]["language"].as_string();
  const std::string title = nb["metadata"]["title"].as_string();
  if (!title.empty()) document.metadata.title = title;
  // nbformat 3 kept cells in worksheets.
  std::vector<const Json*> cells;
  for (const Json& c : nb["cells"].elements()) cells.push_back(&c);
  for (const Json& ws : nb["worksheets"].elements()) {
    for (const Json& c : ws["cells"].elements()) cells.push_back(&c);
  }
  CVT_TRY(ctx.limits.check_archive_entries(cells.size()));
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if ((i & 63) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(cells.size())));
    const Json& cell = *cells[i];
    const std::string type = cell["cell_type"].as_string();
    std::string source = joined(cell.contains("source") ? cell["source"] : cell["input"]);
    if (type == "markdown" || type == "heading") {
      if (type == "heading") source = std::string(static_cast<std::size_t>(std::clamp<std::int64_t>(cell["level"].as_int64(1), 1, 6)), '#') + " " + source;
      add_markdown(source, cell["attachments"], document, ctx);
    } else if (type == "raw") {
      ir::CodeBlock raw{{}, source};
      std::string format = cell["metadata"]["format"].as_string();
      if (format.empty()) format = cell["metadata"]["raw_mimetype"].as_string();
      if (format.find("html") != std::string::npos) raw.raw_format = "html";
      else if (format.find("latex") != std::string::npos) raw.raw_format = "latex";
      else if (format.find("markdown") != std::string::npos) {
        add_markdown(source, Json(), document, ctx);
        continue;
      }
      document.blocks.push_back(std::move(raw));
    } else if (type == "code") {
      while (source.ends_with('\n')) source.pop_back();
      document.blocks.push_back(ir::CodeBlock{language, source});
      for (const Json& output : cell["outputs"].elements()) {
        const std::string output_type = output["output_type"].as_string();
        ir::Attributes attr;
        attr.add_class(ir::conventions::cell_output);
        if (output_type == "stream") {
          std::string text_out = strip_ansi(joined(output.contains("text") ? output["text"] : output["output"]));
          while (text_out.ends_with('\n')) text_out.pop_back();
          if (!text_out.empty()) document.blocks.push_back(ir::Block(ir::CodeBlock{{}, text_out}, std::move(attr)));
        } else if (output_type == "error" || output_type == "pyerr") {
          std::string trace;
          for (const Json& line : output["traceback"].elements()) trace += strip_ansi(line.as_string()) + "\n";
          if (trace.empty()) trace = output["ename"].as_string() + ": " + output["evalue"].as_string();
          while (trace.ends_with('\n')) trace.pop_back();
          document.blocks.push_back(ir::Block(ir::CodeBlock{{}, trace}, std::move(attr)));
        } else if (output_type == "execute_result" || output_type == "display_data" || output_type == "pyout") {
          const Json& data = output.contains("data") ? output["data"] : output;
          bool done = false;
          for (const std::string_view mime : {"image/png", "image/jpeg", "image/gif", "image/svg+xml"}) {
            if (data.contains(mime)) {
              if (auto image = decode_image(data[mime], mime, ctx)) {
                std::string alt = joined(data["text/plain"]);
                document.blocks.push_back(ir::Block(ir::ImageBlock{std::move(*image), alt, {}, {}}, attr));
                done = true;
                break;
              }
            }
          }
          if (done) continue;
          if (data.contains("text/markdown")) {
            add_markdown(joined(data["text/markdown"]), Json(), document, ctx);
          } else if (data.contains("text/html")) {
            HtmlReadOptions html;
            html.read_metadata = false;
            if (auto converted = parse_html_text(joined(data["text/html"]), html)) {
              for (ir::Block& b : converted->blocks) document.blocks.push_back(std::move(b));
            }
          } else if (data.contains("text/latex")) {
            ir::CodeBlock math{{}, joined(data["text/latex"])};
            math.raw_format = "latex";
            document.blocks.push_back(std::move(math));
          } else if (data.contains("text/plain") || data.contains("text")) {
            std::string plain = strip_ansi(joined(data.contains("text/plain") ? data["text/plain"] : data["text"]));
            while (plain.ends_with('\n')) plain.pop_back();
            document.blocks.push_back(ir::Block(ir::CodeBlock{{}, plain}, std::move(attr)));
          }
        }
      }
    }
  }
  if (!document.footnotes.empty()) number_footnotes(document);
  return document;
}

// ---- writing --------------------------------------------------------------------------------------

Json source_lines(std::string_view text) {
  Json lines = Json::array();
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      lines.push_back(std::string(text.substr(start)));
      break;
    }
    lines.push_back(std::string(text.substr(start, nl - start + 1)));
    start = nl + 1;
  }
  return lines;
}

Result<std::string> write_ipynb(const ir::Document& document, WriteContext& ctx) {
  std::string language = ctx.options.get_string("kernel_language", "python");
  if (language.empty()) language = "python";
  Json cells = Json::array();
  ir::Document pending;
  pending.metadata = {};
  int cell_number = 0;
  const auto cell_id = [&](std::string_view content) {
    // Deterministic ids: a hash of the position and content.
    std::string seed = std::to_string(cell_number++) + ":" + std::string(content);
    const auto digest = util::sha256(as_bytes(seed));
    return util::hex_encode(as_bytes(std::span<const std::uint8_t>(digest))).substr(0, 16);
  };
  const auto flush_markdown = [&] {
    if (pending.blocks.empty()) return;
    MarkdownWriteOptions md;
    md.flavor = MarkdownFlavor::Gfm;
    md.front_matter = false;
    md.images = image_handling(ctx.options);
    std::string text = write_markdown(pending, md);
    while (text.ends_with('\n')) text.pop_back();
    Json cell = Json::object();
    cell.set("cell_type", "markdown");
    cell.set("id", cell_id(text));
    cell.set("metadata", Json::object());
    cell.set("source", source_lines(text));
    cells.push_back(std::move(cell));
    pending.blocks.clear();
    pending.footnotes.clear();
  };
  Json* last_code = nullptr;
  // Cells are separate Markdown documents: notes become a closing list.
  ir::Document copy = document;
  footnotes_to_endnotes(copy);
  for (const ir::Block& block : copy.blocks) {
    if (const auto* code = block.get_if<ir::CodeBlock>(); code != nullptr && code->raw_format.empty() && code->math == ir::MathMode::None) {
      if (block.attr.has_class(ir::conventions::cell_output) && last_code != nullptr && pending.blocks.empty()) {
        Json output = Json::object();
        output.set("name", "stdout");
        output.set("output_type", "stream");
        output.set("text", source_lines(code->code + "\n"));
        (*last_code)["outputs"].push_back(std::move(output));
        continue;
      }
      if (util::to_lower(code->language) == util::to_lower(language) || (code->language.empty() && !block.attr.has_class(ir::conventions::cell_output))) {
        flush_markdown();
        Json cell = Json::object();
        cell.set("cell_type", "code");
        cell.set("execution_count", Json());
        cell.set("id", cell_id(code->code));
        cell.set("metadata", Json::object());
        cell.set("outputs", Json::array());
        cell.set("source", source_lines(code->code));
        cells.push_back(std::move(cell));
        last_code = &cells.elements().back();
        continue;
      }
    }
    if (const auto* image = block.get_if<ir::ImageBlock>(); image != nullptr && block.attr.has_class(ir::conventions::cell_output) &&
                                                            last_code != nullptr && pending.blocks.empty()) {
      if (auto encoded = encoded_image(image->source)) {
        Json data = Json::object();
        data.set(encoded->mime, util::base64_encode(encoded->bytes));
        data.set("text/plain", image->alt);
        Json output = Json::object();
        output.set("data", std::move(data));
        output.set("metadata", Json::object());
        output.set("output_type", "display_data");
        (*last_code)["outputs"].push_back(std::move(output));
        continue;
      }
    }
    last_code = nullptr;
    pending.blocks.push_back(block);
  }
  flush_markdown();
  // Unreferenced notes in a cell are dropped by the Markdown writer only if
  // never referenced; they are harmless otherwise.
  Json nb = Json::object();
  nb.set("cells", std::move(cells));
  Json metadata = Json::object();
  Json kernelspec = Json::object();
  kernelspec.set("display_name", language == "python" ? "Python 3" : language);
  kernelspec.set("language", language);
  kernelspec.set("name", language == "python" ? "python3" : language);
  metadata.set("kernelspec", std::move(kernelspec));
  Json info = Json::object();
  info.set("name", language);
  metadata.set("language_info", std::move(info));
  if (!document.metadata.title.empty()) metadata.set("title", document.metadata.title);
  nb.set("metadata", std::move(metadata));
  nb.set("nbformat", 4);
  nb.set("nbformat_minor", 5);
  return util::dump_json(nb, 1) + "\n";
}

int sniff_ipynb(ByteView data, std::string_view filename) {
  const std::string_view head = sniff_head(data, 65536);
  const bool content = util::trim_left(head).starts_with("{") && head.find("\"nbformat\"") != std::string_view::npos;
  if (content) return has_extension(filename, {"ipynb"}) ? 95 : 85;
  if (has_extension(filename, {"ipynb"}) && util::trim_left(head).starts_with("{")) return 70;
  return 0;
}

}  // namespace

void register_ipynb(Registry& registry) {
  MarkupFormat nb;
  nb.id = "ipynb";
  nb.name = "Jupyter notebook";
  nb.description = "Jupyter notebook: Markdown, code cells and their outputs.";
  nb.extensions = {"ipynb"};
  nb.mime = "application/x-ipynb+json";
  nb.popularity = 45;
  nb.sniff = sniff_ipynb;
  nb.read = read_ipynb;
  nb.write = write_ipynb;
  nb.write_options = {OptionSpec::text("kernel_language", "Notebook language", "python",
                                       "Code blocks in this language become code cells; others stay in Markdown cells.")};
  register_markup(registry, std::move(nb));
}

}  // namespace cvt::document
