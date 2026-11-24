// Convertible core - document family: bridges between Document and the
// PlainText, Table and Data kinds, including the JSON document AST.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "families/document/formats.hpp"
#include "ir/any.hpp"
#include "util/base64.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

// =========================================================================== JSON AST

constexpr int ast_version = 1;

std::string_view align_name(ir::Alignment a) {
  switch (a) {
    case ir::Alignment::Left: return "left";
    case ir::Alignment::Center: return "center";
    case ir::Alignment::Right: return "right";
    case ir::Alignment::Default: return "default";
  }
  return "default";
}

ir::Alignment align_from(std::string_view s) {
  if (s == "left") return ir::Alignment::Left;
  if (s == "center") return ir::Alignment::Center;
  if (s == "right") return ir::Alignment::Right;
  return ir::Alignment::Default;
}

std::string_view math_name(ir::MathMode m) {
  switch (m) {
    case ir::MathMode::Inline: return "inline";
    case ir::MathMode::Display: return "display";
    case ir::MathMode::None: return "none";
  }
  return "none";
}

ir::MathMode math_from(std::string_view s) {
  if (s == "inline") return ir::MathMode::Inline;
  if (s == "display") return ir::MathMode::Display;
  return ir::MathMode::None;
}

std::string_view numbering_name(ir::ListNumbering n) {
  switch (n) {
    case ir::ListNumbering::LowerAlpha: return "lower-alpha";
    case ir::ListNumbering::UpperAlpha: return "upper-alpha";
    case ir::ListNumbering::LowerRoman: return "lower-roman";
    case ir::ListNumbering::UpperRoman: return "upper-roman";
    case ir::ListNumbering::Decimal: return "decimal";
  }
  return "decimal";
}

ir::ListNumbering numbering_from(std::string_view s) {
  if (s == "lower-alpha") return ir::ListNumbering::LowerAlpha;
  if (s == "upper-alpha") return ir::ListNumbering::UpperAlpha;
  if (s == "lower-roman") return ir::ListNumbering::LowerRoman;
  if (s == "upper-roman") return ir::ListNumbering::UpperRoman;
  return ir::ListNumbering::Decimal;
}

Json attr_json(const ir::Attributes& attr) {
  Json out = Json::object();
  if (!attr.id.empty()) out.set("id", attr.id);
  if (!attr.classes.empty()) {
    Json classes = Json::array();
    for (const std::string& c : attr.classes) classes.push_back(c);
    out.set("classes", std::move(classes));
  }
  if (!attr.pairs.empty()) {
    Json pairs = Json::object();
    for (const auto& [k, v] : attr.pairs) pairs.set(k, v);
    out.set("attributes", std::move(pairs));
  }
  return out;
}

ir::Attributes attr_from(const Json& j) {
  ir::Attributes attr;
  if (!j.is_object()) return attr;
  attr.id = j["id"].as_string();
  for (const Json& c : j["classes"].elements()) attr.classes.push_back(c.as_string());
  for (const auto& member : j["attributes"].items()) attr.pairs.emplace_back(member.key(), member.value().as_string());
  return attr;
}

Json source_json(const ir::ImageSource& source) {
  Json out = Json::object();
  if (const auto* e = std::get_if<ir::EncodedImage>(&source); e != nullptr && !e->url.empty()) out.set("url", e->url);
  if (const auto encoded = encoded_image(source)) {
    out.set("mime", encoded->mime);
    out.set("data", util::base64_encode(encoded->bytes));
  }
  return out;
}

ir::ImageSource source_from(const Json& j) {
  ir::EncodedImage image;
  image.url = j["url"].as_string();
  image.mime = j["mime"].as_string();
  if (const std::string& data = j["data"].as_string(); !data.empty()) {
    if (auto bytes = util::base64_decode(data)) image.bytes = std::move(*bytes);
  }
  return image;
}

Json inlines_json(const ir::Inlines& inlines, int depth);

Json inline_json(const ir::Inline& item, int depth) {
  Json out = Json::object();
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        const auto children = [&](std::string_view type) {
          out.set("type", type);
          if constexpr (requires { node.children; }) out.set("children", inlines_json(node.children, depth + 1));
        };
        if constexpr (std::same_as<T, ir::Text>) {
          out.set("type", "text");
          out.set("text", node.text);
        } else if constexpr (std::same_as<T, ir::Emphasis>) {
          children("emphasis");
        } else if constexpr (std::same_as<T, ir::Strong>) {
          children("strong");
        } else if constexpr (std::same_as<T, ir::Strikethrough>) {
          children("strikethrough");
        } else if constexpr (std::same_as<T, ir::Underline>) {
          children("underline");
        } else if constexpr (std::same_as<T, ir::Superscript>) {
          children("superscript");
        } else if constexpr (std::same_as<T, ir::Subscript>) {
          children("subscript");
        } else if constexpr (std::same_as<T, ir::Code>) {
          out.set("type", "code");
          out.set("text", node.text);
          if (node.math != ir::MathMode::None) out.set("math", math_name(node.math));
          if (!node.raw_format.empty()) out.set("raw_format", node.raw_format);
        } else if constexpr (std::same_as<T, ir::Link>) {
          out.set("type", "link");
          out.set("url", node.url);
          if (!node.title.empty()) out.set("title", node.title);
          if (!node.note.empty()) out.set("note", node.note);
          out.set("children", inlines_json(node.children, depth + 1));
        } else if constexpr (std::same_as<T, ir::LineBreak>) {
          out.set("type", "line_break");
        } else if constexpr (std::same_as<T, ir::InlineImage>) {
          out.set("type", "image");
          out.set("alt", node.alt);
          if (!node.title.empty()) out.set("title", node.title);
          out.set("source", source_json(node.source));
        }
      },
      item.node);
  if (!item.attr.empty()) out.set("attr", attr_json(item.attr));
  return out;
}

Json inlines_json(const ir::Inlines& inlines, int depth) {
  Json out = Json::array();
  if (depth > ir::max_document_depth * 2) return out;
  for (const ir::Inline& item : inlines) out.push_back(inline_json(item, depth));
  return out;
}

Json blocks_json(const ir::Blocks& blocks, int depth);

Json block_json(const ir::Block& block, int depth) {
  Json out = Json::object();
  std::visit(
      [&](const auto& node) {
        using T = std::remove_cvref_t<decltype(node)>;
        if constexpr (std::same_as<T, ir::Paragraph>) {
          out.set("type", "paragraph");
          out.set("content", inlines_json(node.content, depth));
        } else if constexpr (std::same_as<T, ir::Heading>) {
          out.set("type", "heading");
          out.set("level", node.level);
          out.set("content", inlines_json(node.content, depth));
        } else if constexpr (std::same_as<T, ir::List>) {
          out.set("type", "list");
          out.set("ordered", node.ordered);
          if (node.ordered) {
            out.set("start", node.start);
            out.set("numbering", numbering_name(node.numbering));
          }
          if (node.definition) out.set("definition", true);
          if (node.loose) out.set("loose", true);
          if (!node.tasks.empty()) {
            Json tasks = Json::array();
            for (const ir::TaskState t : node.tasks) {
              tasks.push_back(t == ir::TaskState::Done ? "done" : t == ir::TaskState::Open ? "open" : "none");
            }
            out.set("tasks", std::move(tasks));
          }
          Json items = Json::array();
          for (const ir::Blocks& item : node.items) items.push_back(blocks_json(item, depth + 1));
          out.set("items", std::move(items));
        } else if constexpr (std::same_as<T, ir::CodeBlock>) {
          out.set("type", "code_block");
          if (!node.language.empty()) out.set("language", node.language);
          out.set("code", node.code);
          if (node.math != ir::MathMode::None) out.set("math", math_name(node.math));
          if (!node.raw_format.empty()) out.set("raw_format", node.raw_format);
        } else if constexpr (std::same_as<T, ir::BlockQuote>) {
          out.set("type", "block_quote");
          out.set("blocks", blocks_json(node.blocks, depth + 1));
        } else if constexpr (std::same_as<T, ir::TableBlock>) {
          out.set("type", "table");
          if (!node.caption.empty()) out.set("caption", inlines_json(node.caption, depth));
          if (!node.alignments.empty()) {
            Json aligns = Json::array();
            for (const ir::Alignment a : node.alignments) aligns.push_back(align_name(a));
            out.set("alignments", std::move(aligns));
          }
          Json rows = Json::array();
          for (const ir::TableRow& row : node.rows) {
            Json r = Json::object();
            if (row.header) r.set("header", true);
            Json cells = Json::array();
            for (const ir::TableCell& cell : row.cells) {
              Json c = Json::object();
              c.set("content", inlines_json(cell.content, depth));
              if (cell.colspan != 1) c.set("colspan", cell.colspan);
              if (cell.rowspan != 1) c.set("rowspan", cell.rowspan);
              if (cell.align != ir::Alignment::Default) c.set("align", align_name(cell.align));
              cells.push_back(std::move(c));
            }
            r.set("cells", std::move(cells));
            rows.push_back(std::move(r));
          }
          out.set("rows", std::move(rows));
        } else if constexpr (std::same_as<T, ir::ImageBlock>) {
          out.set("type", "image");
          out.set("alt", node.alt);
          if (!node.caption.empty()) out.set("caption", node.caption);
          if (!node.title.empty()) out.set("title", node.title);
          out.set("source", source_json(node.source));
        } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
          out.set("type", "horizontal_rule");
        } else if constexpr (std::same_as<T, ir::PageBreak>) {
          out.set("type", "page_break");
        }
      },
      block.node);
  if (!block.attr.empty()) out.set("attr", attr_json(block.attr));
  return out;
}

Json blocks_json(const ir::Blocks& blocks, int depth) {
  Json out = Json::array();
  if (depth > ir::max_document_depth * 2) return out;
  for (const ir::Block& block : blocks) out.push_back(block_json(block, depth));
  return out;
}

Json document_json(const ir::Document& document) {
  Json out = Json::object();
  out.set("type", "document");
  out.set("version", ast_version);
  Json meta = Json::object();
  const ir::DocumentMetadata& m = document.metadata;
  if (!m.title.empty()) meta.set("title", m.title);
  if (!m.author.empty()) meta.set("author", m.author);
  if (!m.language.empty()) meta.set("language", m.language);
  if (!m.date.empty()) meta.set("date", m.date);
  if (!m.extra.empty()) {
    Json extra = Json::object();
    for (const auto& [k, v] : m.extra) extra.set(k, v);
    meta.set("extra", std::move(extra));
  }
  out.set("metadata", std::move(meta));
  out.set("blocks", blocks_json(document.blocks, 0));
  if (!document.footnotes.empty()) {
    Json notes = Json::array();
    for (const ir::Footnote& note : document.footnotes) {
      Json n = Json::object();
      n.set("label", note.label);
      n.set("blocks", blocks_json(note.blocks, 1));
      notes.push_back(std::move(n));
    }
    out.set("footnotes", std::move(notes));
  }
  return out;
}

// ---- AST -> Document ----

ir::Inlines inlines_from(const Json& j, int depth);

ir::Inlines inlines_from(const Json& j, int depth) {
  ir::Inlines out;
  if (depth > ir::max_document_depth) {
    // Too deep: keep the text only.
    std::vector<const Json*> stack{&j};
    while (!stack.empty()) {
      const Json* node = stack.back();
      stack.pop_back();
      if (node->is_array()) {
        for (auto it = node->elements().rbegin(); it != node->elements().rend(); ++it) stack.push_back(&*it);
      } else if (node->is_object()) {
        if ((*node)["type"].as_string() == "text") append_text(out, (*node)["text"].as_string());
        stack.push_back(&(*node)["children"]);
      }
    }
    return out;
  }
  for (const Json& item : j.elements()) {
    const std::string& type = item["type"].as_string();
    const auto kids = [&] { return inlines_from(item["children"], depth + 1); };
    ir::Inline node;
    if (type == "text") {
      node = ir::Text{item["text"].as_string()};
    } else if (type == "emphasis") {
      node = ir::Emphasis{kids()};
    } else if (type == "strong") {
      node = ir::Strong{kids()};
    } else if (type == "strikethrough") {
      node = ir::Strikethrough{kids()};
    } else if (type == "underline") {
      node = ir::Underline{kids()};
    } else if (type == "superscript") {
      node = ir::Superscript{kids()};
    } else if (type == "subscript") {
      node = ir::Subscript{kids()};
    } else if (type == "code") {
      ir::Code code{item["text"].as_string()};
      code.math = math_from(item["math"].as_string());
      code.raw_format = item["raw_format"].as_string();
      node = std::move(code);
    } else if (type == "link") {
      ir::Link link{item["url"].as_string(), item["title"].as_string(), kids()};
      link.note = item["note"].as_string();
      node = std::move(link);
    } else if (type == "line_break") {
      node = ir::LineBreak{};
    } else if (type == "image") {
      node = ir::InlineImage{source_from(item["source"]), item["alt"].as_string(), item["title"].as_string()};
    } else {
      continue;
    }
    node.attr = attr_from(item["attr"]);
    out.push_back(std::move(node));
  }
  return out;
}

ir::Blocks blocks_from(const Json& j, int depth) {
  ir::Blocks out;
  if (depth > ir::max_document_depth) return out;
  for (const Json& item : j.elements()) {
    const std::string& type = item["type"].as_string();
    ir::Block block;
    if (type == "paragraph") {
      block = ir::Paragraph{inlines_from(item["content"], 0)};
    } else if (type == "heading") {
      block = ir::Heading{static_cast<int>(std::clamp<std::int64_t>(item["level"].as_int64(1), 1, 6)),
                          inlines_from(item["content"], 0)};
    } else if (type == "list") {
      ir::List list;
      list.ordered = item["ordered"].as_bool();
      list.start = static_cast<int>(std::clamp<std::int64_t>(item["start"].as_int64(1), -1000000000, 1000000000));
      list.numbering = numbering_from(item["numbering"].as_string());
      list.definition = item["definition"].as_bool();
      list.loose = item["loose"].as_bool();
      for (const Json& t : item["tasks"].elements()) {
        const std::string& s = t.as_string();
        list.tasks.push_back(s == "done" ? ir::TaskState::Done : s == "open" ? ir::TaskState::Open : ir::TaskState::None);
      }
      for (const Json& entry : item["items"].elements()) list.items.push_back(blocks_from(entry, depth + 1));
      if (!list.tasks.empty()) list.tasks.resize(list.items.size(), ir::TaskState::None);
      block = std::move(list);
    } else if (type == "code_block") {
      ir::CodeBlock code{item["language"].as_string(), item["code"].as_string()};
      code.math = math_from(item["math"].as_string());
      code.raw_format = item["raw_format"].as_string();
      block = std::move(code);
    } else if (type == "block_quote") {
      block = ir::BlockQuote{blocks_from(item["blocks"], depth + 1)};
    } else if (type == "table") {
      ir::TableBlock table;
      table.caption = inlines_from(item["caption"], 0);
      for (const Json& a : item["alignments"].elements()) table.alignments.push_back(align_from(a.as_string()));
      for (const Json& r : item["rows"].elements()) {
        ir::TableRow row;
        row.header = r["header"].as_bool();
        for (const Json& c : r["cells"].elements()) {
          ir::TableCell cell;
          cell.content = inlines_from(c["content"], 0);
          cell.colspan = static_cast<int>(std::clamp<std::int64_t>(c["colspan"].as_int64(1), 1, 1000));
          cell.rowspan = static_cast<int>(std::clamp<std::int64_t>(c["rowspan"].as_int64(1), 1, 1000));
          cell.align = align_from(c["align"].as_string());
          row.cells.push_back(std::move(cell));
        }
        table.rows.push_back(std::move(row));
      }
      block = std::move(table);
    } else if (type == "image") {
      block = ir::ImageBlock{source_from(item["source"]), item["alt"].as_string(), item["caption"].as_string(),
                             item["title"].as_string()};
    } else if (type == "horizontal_rule") {
      block = ir::HorizontalRule{};
    } else if (type == "page_break") {
      block = ir::PageBreak{};
    } else {
      continue;
    }
    block.attr = attr_from(item["attr"]);
    out.push_back(std::move(block));
  }
  return out;
}

bool is_document_ast(const Json& j) { return j.is_object() && j["type"].as_string() == "document" && j["blocks"].is_array(); }

ir::Document document_from(const Json& j) {
  ir::Document document;
  const Json& m = j["metadata"];
  document.metadata.title = m["title"].as_string();
  document.metadata.author = m["author"].as_string();
  document.metadata.language = m["language"].as_string();
  document.metadata.date = m["date"].as_string();
  for (const auto& member : m["extra"].items()) document.metadata.extra[member.key()] = member.value().as_string();
  document.blocks = blocks_from(j["blocks"], 0);
  for (const Json& n : j["footnotes"].elements()) {
    document.footnotes.push_back(ir::Footnote{n["label"].as_string(), blocks_from(n["blocks"], 1)});
  }
  return document;
}

// ---- generic data -> readable document ----

std::string scalar_text(const Json& j) {
  if (j.is_string()) return j.as_string();
  if (j.is_null()) return "null";
  return util::dump_json(j);
}

bool is_scalar(const Json& j) { return !j.is_structured(); }

// An array of objects with scalar values renders best as a table.
bool tabular(const Json& j) {
  if (!j.is_array() || j.empty() || j.size() > 10000) return false;
  for (const Json& row : j.elements()) {
    if (!row.is_object()) return false;
    for (const auto& member : row.items()) {
      if (!is_scalar(member.value())) return false;
    }
  }
  return true;
}

void render_data(const Json& j, ir::Blocks& out, int depth) {
  if (depth > 24) {
    out.push_back(ir::CodeBlock{"json", util::dump_json(j, 2)});
    return;
  }
  if (is_scalar(j)) {
    out.push_back(ir::Paragraph{{ir::Text{scalar_text(j)}}});
    return;
  }
  if (tabular(j)) {
    std::vector<std::string> columns;
    for (const Json& row : j.elements()) {
      for (const auto& member : row.items()) {
        if (std::ranges::find(columns, member.key()) == columns.end()) columns.push_back(member.key());
      }
    }
    ir::TableBlock table;
    table.rows.push_back(ir::build::row(columns, true));
    for (const Json& row : j.elements()) {
      std::vector<std::string> cells;
      for (const std::string& c : columns) cells.push_back(row.contains(c) ? scalar_text(row[c]) : std::string());
      table.rows.push_back(ir::build::row(std::move(cells)));
    }
    out.push_back(std::move(table));
    return;
  }
  if (j.is_array()) {
    ir::List list;
    for (const Json& element : j.elements()) {
      ir::Blocks item;
      render_data(element, item, depth + 1);
      list.items.push_back(std::move(item));
    }
    out.push_back(std::move(list));
    return;
  }
  // Object: a definition list of keys.
  ir::List list;
  list.definition = true;
  for (const auto& member : j.items()) {
    ir::Blocks item;
    item.push_back(ir::Paragraph{{ir::Strong{{ir::Text{member.key()}}}}});
    render_data(member.value(), item, depth + 1);
    list.items.push_back(std::move(item));
  }
  out.push_back(std::move(list));
}

// =========================================================================== Table extraction

ir::Cell typed_cell(std::string text) {
  const std::string_view t = util::trim(text);
  if (t.empty()) return {};
  if (!t.empty() && (util::is_ascii_digit(t.front()) || t.front() == '-' || t.front() == '+') &&
      t.find_first_not_of("0123456789+-.eE") == std::string_view::npos) {
    if (const auto value = util::parse_double(t)) return ir::Cell::numeric(*value, std::string(t));
  }
  return ir::Cell::string(std::move(text));
}

void collect_tables(const ir::Blocks& blocks, std::vector<const ir::TableBlock*>& out, int depth) {
  if (depth > ir::max_document_depth * 2) return;
  for (const ir::Block& block : blocks) {
    if (const auto* t = block.get_if<ir::TableBlock>()) out.push_back(t);
    if (const auto* l = block.get_if<ir::List>()) {
      for (const ir::Blocks& item : l->items) collect_tables(item, out, depth + 1);
    }
    if (const auto* q = block.get_if<ir::BlockQuote>()) collect_tables(q->blocks, out, depth + 1);
  }
}

Result<AnyIR> document_to_table(AnyIR value, Context& ctx) {
  const ir::Document document = std::move(value).get<ir::Document>();
  std::vector<const ir::TableBlock*> tables;
  collect_tables(document.blocks, tables, 0);
  if (tables.empty()) return fail(Status::Encode, "the document contains no tables");
  const bool all = ctx.options.get_enum("tables", "all", {"first", "all"}) == "all";
  if (!all) tables.resize(1);
  ir::Table out;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(tables.size()), "Extracting tables"));
    const ir::TableBlock normalized = normalize_table(*tables[i]);
    ir::Sheet sheet;
    sheet.name = tables[i]->caption.empty() ? "Table " + std::to_string(i + 1) : inline_line(tables[i]->caption);
    if (sheet.name.size() > 31) sheet.name.resize(31);
    sheet.has_header = !normalized.rows.empty() && normalized.rows.front().header;
    for (const ir::TableRow& row : normalized.rows) {
      std::vector<ir::Cell> cells;
      for (const ir::TableCell& cell : row.cells) {
        std::string text = ir::plain_text(cell.content);
        cells.push_back(row.header ? (text.empty() ? ir::Cell{} : ir::Cell::string(std::move(text)))
                                   : typed_cell(std::move(text)));
      }
      sheet.rows.push_back(std::move(cells));
    }
    out.sheets.push_back(std::move(sheet));
  }
  if (tables.size() > 1 && !all) ctx.warn("Only the first table was extracted.");
  return out;
}

// =========================================================================== bridges

Result<AnyIR> document_to_plain_text(AnyIR value, Context& ctx) {
  const ir::Document document = std::move(value).get<ir::Document>();
  PlainTextOptions options;
  options.wrap_width = static_cast<int>(ctx.options.get_int("wrap_width", 72, 0, 1000));
  options.underline_headings = ctx.options.get_bool("underline_headings", true);
  options.ascii_tables = ctx.options.get_bool("ascii_tables", true);
  options.link_references = ctx.options.get_bool("link_references", true);
  ir::PlainText text;
  text.text = to_plain_text(document, options);
  return text;
}

Result<AnyIR> plain_text_to_document(AnyIR value, Context& ctx) {
  const ir::PlainText text = std::move(value).get<ir::PlainText>();
  CVT_TRY(ctx.checkpoint(0.0, "Reading text"));
  if (!ctx.options.get_bool("infer_structure", true)) {
    // One paragraph per block of non-blank lines (the plain behaviour).
    TextInferenceOptions none;
    none.headings = none.lists = none.code = none.quotes = none.rules = false;
    return infer_document_from_text(text.text, none);
  }
  ir::Document document = infer_document_from_text(text.text);
  enforce_depth(document);
  return document;
}

Result<AnyIR> document_to_data(AnyIR value, Context& ctx) {
  CVT_TRY(ctx.checkpoint(0.0, "Converting"));
  const ir::Document document = std::move(value).get<ir::Document>();
  ir::Data data;
  data.value = document_json(document);
  return data;
}

Result<AnyIR> data_to_document(AnyIR value, Context& ctx) {
  CVT_TRY(ctx.checkpoint(0.0, "Converting"));
  const ir::Data data = std::move(value).get<ir::Data>();
  if (is_document_ast(data.value)) {
    ir::Document document = document_from(data.value);
    enforce_depth(document);
    normalize_document(document);
    return document;
  }
  ctx.warn("The data is not a document syntax tree; it was laid out as lists and tables.");
  ir::Document document;
  render_data(data.value, document.blocks, 0);
  enforce_depth(document);
  return document;
}

}  // namespace

void register_bridges(Registry& registry) {
  Bridge to_text;
  to_text.from = Kind::Document;
  to_text.to = Kind::PlainText;
  to_text.fn = document_to_plain_text;
  to_text.cost = 20;
  to_text.lossy = true;
  to_text.label = "lay out document as plain text";
  to_text.options = {
      option_wrap_width(72),
      OptionSpec::boolean("underline_headings", "Underline headings", true,
                          "Underline level 1 and 2 headings with = and - characters."),
      OptionSpec::boolean("ascii_tables", "Table borders", true, "Draw tables with +---+ borders; off separates cells by tabs."),
      OptionSpec::boolean("link_references", "Numbered links", true,
                          "List link addresses at the end as [1], [2], ...; off writes them inline in <>."),
  };
  registry.add_bridge(std::move(to_text));

  Bridge from_text;
  from_text.from = Kind::PlainText;
  from_text.to = Kind::Document;
  from_text.fn = plain_text_to_document;
  from_text.cost = 14;  // preferred over the encoding family's paragraph-only bridge
  // Indentation, runs of blank lines and trailing blanks are not kept.
  from_text.lossy = true;
  from_text.label = "recognise headings, lists and code in plain text";
  from_text.options = {OptionSpec::boolean("infer_structure", "Detect structure", true,
                                           "Recognise underlined and numbered headings, bulleted and numbered lists, "
                                           "indented code and quotes; off makes every block a paragraph.")};
  registry.add_bridge(std::move(from_text));

  Bridge to_table;
  to_table.from = Kind::Document;
  to_table.to = Kind::Table;
  to_table.fn = document_to_table;
  to_table.cost = 25;
  to_table.lossy = true;
  to_table.label = "extract the tables of a document";
  to_table.after_reader_only = true;  // only documents read as such have tables
  // Source code read as a document is one code block: no SQL dump of it.
  to_table.excluded_target_categories = {Category::Code};
  to_table.options = {OptionSpec::choice("tables", "Tables", "all", {"first", "all"},
                                         "Extract every table (one sheet each) or only the first one.")};
  registry.add_bridge(std::move(to_table));

  Bridge to_data;
  to_data.from = Kind::Document;
  to_data.to = Kind::Data;
  to_data.fn = document_to_data;
  to_data.cost = 15;
  to_data.label = "document syntax tree as JSON";
  registry.add_bridge(std::move(to_data));

  Bridge from_data;
  from_data.from = Kind::Data;
  from_data.to = Kind::Document;
  from_data.fn = data_to_document;
  from_data.cost = 20;
  from_data.lossy = true;  // exact for the document syntax tree, a layout for other data
  from_data.label = "document from its JSON syntax tree (or data laid out as lists and tables)";
  registry.add_bridge(std::move(from_data));
}

}  // namespace cvt::document
