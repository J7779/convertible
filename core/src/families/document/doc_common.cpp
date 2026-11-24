// Convertible core - document family: shared registration pipeline, option
// handling, charsets, inline rendering and parsing helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/document/doc_common.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

#include "engine/standard_options.hpp"
#include "ir/any.hpp"
#include "util/crc.hpp"
#include "util/deflate.hpp"
#include "util/charsets.hpp"
#include "util/text.hpp"

namespace cvt::document {

// =========================================================================== options

OptionSpec option_wrap_width(int default_width) {
  return OptionSpec::integer("wrap_width", "Wrap width", default_width, 0, 1000,
                             "Wrap paragraphs at this many columns; 0 keeps every paragraph on one line.");
}

OptionSpec option_standalone() {
  return OptionSpec::boolean("standalone", "Standalone document", true,
                             "Write a complete document (header, metadata, styles); off writes only the body.");
}

std::vector<OptionSpec> common_write_options() {
  return {
      options::strip_metadata(),
      OptionSpec::boolean("toc", "Table of contents", false, "Insert a table of contents built from the headings."),
      OptionSpec::integer("toc_depth", "Contents depth", 3, 1, 6, "Deepest heading level listed in the contents.")
          .advanced(),
      OptionSpec::integer("heading_shift", "Heading level shift", 0, -5, 5,
                          "Add this to every heading level (e.g. 1 turns titles into sections).")
          .advanced(),
      OptionSpec::choice("images", "Images", "embed", {"embed", "link", "drop"},
                         "Embed pictures in the output, keep only references to them, or leave them out."),
      OptionSpec::choice("line_endings", "Line endings", "lf", {"lf", "crlf", "cr"},
                         "Line terminator of the output: lf (Unix), crlf (Windows) or cr (classic Mac).")
          .advanced(),
  };
}

ImageHandling image_handling(const Options& options) {
  const std::string value = options.get_enum("images", "embed", {"embed", "link", "drop"});
  if (value == "link") return ImageHandling::Link;
  if (value == "drop") return ImageHandling::Drop;
  return ImageHandling::Embed;
}

std::string newline_for(const Options& options) {
  const std::string value = options.get_enum("line_endings", "lf", {"lf", "crlf", "cr", "keep"});
  if (value == "crlf") return "\r\n";
  if (value == "cr") return "\r";
  return "\n";
}

void apply_write_options(ir::Document& document, const Options& options) {
  if (options::resolved_strip_metadata(options)) strip_metadata(document);
  const auto shift = static_cast<int>(options.get_int("heading_shift", 0, -5, 5));
  if (shift != 0) shift_headings(document, shift);
  const ImageHandling images = image_handling(options);
  if (images != ImageHandling::Embed) apply_image_handling(document, images);
  if (options.get_bool("toc", false)) {
    insert_table_of_contents(document, static_cast<int>(options.get_int("toc_depth", 3, 1, 6)));
  }
}

// =========================================================================== charsets

namespace {

std::string normalize_label(std::string_view label) {
  std::string out;
  for (const char c : util::trim(label)) {
    if (c == '_') {
      out.push_back('-');
    } else if (c != '"' && c != '\'') {
      out.push_back(util::ascii_lower(c));
    }
  }
  return out;
}

// WHATWG labels the shared charset module does not spell the same way.
std::string_view whatwg_alias(std::string_view name) {
  if (name == "iso-8859-8-i" || name == "csiso88598i" || name == "logical" || name == "visual") return "iso-8859-8";
  if (name == "koi8-ru" || name == "koi") return name == "koi" ? "koi8-r" : "koi8-u";
  if (name == "x-sjis" || name == "ms-kanji") return "shift_jis";
  if (name == "x-euc-jp") return "euc-jp";
  if (name == "x-gbk" || name == "gb-2312" || name == "chinese") return "gbk";
  if (name == "big5-hkscs" || name == "cn-big5") return "big5";
  if (name == "ks-c-5601-1987" || name == "windows-949") return "euc-kr";
  return name;
}

}  // namespace

bool is_known_charset(std::string_view label) {
  const std::string name = normalize_label(label);
  if (name == "us-ascii" || name == "ascii" || name == "ansi-x3.4-1968") return true;
  if (util::parse_encoding(name)) return true;
  return util::decode_labelled({}, whatwg_alias(name)).has_value();
}

bool decode_charset(ByteView data, std::string_view label, std::string& out) {
  // Every charset of util/charsets.hpp: Unicode, the single-byte code pages
  // and the East Asian multi-byte encodings (Shift_JIS, GBK, Big5, EUC-KR ...).
  const std::string name = normalize_label(label);
  auto decoded = util::decode_labelled(data, whatwg_alias(name));
  if (!decoded) return false;
  out = std::move(decoded->text);
  return true;
}

std::string decode_document_text(ByteView data, const Context& ctx, std::string_view requested) {
  if (!requested.empty() && requested != "auto") {
    std::string out;
    if (decode_charset(data, requested, out)) return out;
    ctx.warn("Unknown source encoding '" + std::string(requested) + "'; the encoding was detected instead.");
  }
  const util::EncodingGuess guess = util::guess_encoding(data);
  util::CharsetText decoded;
  if (guess.confident) {
    util::DecodedText unicode = util::decode_text(data, guess.encoding);
    decoded.text = std::move(unicode.text);
    decoded.replaced = unicode.replaced;
  } else {
    // Not Unicode: a legacy charset the shared detector is confident about
    // (East Asian multi-byte text decodes cleanly in only one), else Windows-1252.
    decoded = util::decode_auto(data, "windows-1252");
    if (decoded.charset == "windows-1252") {
      ctx.warn("The text is not valid Unicode; it was read as Windows-1252. Set 'source_encoding' if that is wrong.");
    } else {
      ctx.warn("The text is not valid Unicode; it was read as " + decoded.charset + ". Set 'source_encoding' if that is wrong.");
    }
  }
  if (decoded.replaced > 0) {
    ctx.warn(std::to_string(decoded.replaced) + " malformed byte sequence(s) were replaced with U+FFFD.");
  }
  // Keep NULs out of documents: they confuse every text format.
  std::erase(decoded.text, '\0');
  return std::move(decoded.text);
}

// =========================================================================== registration

namespace {

std::vector<std::string> source_encoding_choices() {
  std::vector<std::string> out = {"auto", "utf-8", "utf-16le", "utf-16be"};
  for (const util::CharsetInfo& charset : util::legacy_charsets()) out.emplace_back(charset.id);
  return out;
}

}  // namespace

void register_markup(Registry& registry, MarkupFormat format) {
  FormatInfo info;
  info.id = format.id;
  info.name = format.name;
  info.description = format.description;
  info.category = format.category;
  info.extensions = format.extensions;
  info.mime = format.mime;
  info.popularity = format.popularity;
  info.signatures = format.signatures;
  info.sniff = format.sniff;
  registry.add_format(std::move(info));

  if (format.read || format.read_bytes) {
    Codec reader;
    reader.format_id = format.id;
    reader.kind = Kind::Document;
    reader.read_cost = format.read_cost;
    reader.lossy = format.read_lossy;
    reader.read_options = format.read_options;
    const bool text_reader = !format.read_bytes;
    if (text_reader && format.text_options) {
      reader.read_options.push_back(
          OptionSpec::choice("source_encoding", "Source encoding", "auto", source_encoding_choices(),
                             "Character encoding of the input; 'auto' detects Unicode and falls back to Windows-1252.")
              .advanced());
    }
    reader.read = [read = format.read, read_bytes = format.read_bytes](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY(ctx.checkpoint(0.0, "Reading"));
      CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 4));
      ir::Document document;
      if (read_bytes) {
        CVT_TRY_ASSIGN(document, read_bytes(ctx.input, ctx));
      } else {
        const std::string text = decode_document_text(ctx.input, ctx, ctx.options.get_string("source_encoding", "auto"));
        CVT_TRY_ASSIGN(document, read(text, ctx));
      }
      enforce_depth(document);
      normalize_document(document);
      CVT_TRY(ctx.checkpoint(1.0, "Reading"));
      return document;
    };
    registry.add_codec(std::move(reader));
  }

  if (format.write) {
    Codec writer;
    writer.format_id = format.id;
    writer.kind = Kind::Document;
    writer.write_cost = format.write_cost;
    writer.lossy = format.write_lossy;
    writer.write_options = format.write_options;
    if ((format.features & feature_wrap) != 0) writer.write_options.push_back(option_wrap_width(format.default_wrap));
    if ((format.features & feature_standalone) != 0) writer.write_options.push_back(option_standalone());
    for (OptionSpec& spec : common_write_options()) writer.write_options.push_back(std::move(spec));
    writer.write = [write = format.write, binary = format.binary_output](const AnyIR& value,
                                                                        WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY(ctx.checkpoint(0.0, "Writing"));
      ir::Document document = value.get<ir::Document>();
      apply_write_options(document, ctx.options);
      CVT_TRY_ASSIGN(std::string text, write(document, ctx));
      if (!binary) {
        const std::string newline = newline_for(ctx.options);
        if (newline != "\n") {
          text = util::convert_line_endings(text, newline == "\r\n" ? util::LineEnding::CrLf : util::LineEnding::Cr);
        }
      }
      CVT_TRY(ctx.limits.check_output(text.size()));
      CVT_TRY(ctx.checkpoint(1.0, "Writing"));
      return to_bytes(text);
    };
    registry.add_codec(std::move(writer));
  }
}

// =========================================================================== inline rendering

std::size_t footnote_index(const ir::Document& document, std::string_view label) {
  for (std::size_t i = 0; i < document.footnotes.size(); ++i) {
    if (document.footnotes[i].label == label) return i + 1;
  }
  return 0;
}

const ir::Footnote* find_footnote(const ir::Document& document, std::string_view label) {
  for (const ir::Footnote& note : document.footnotes) {
    if (note.label == label) return &note;
  }
  return nullptr;
}

namespace {

void render_inlines_into(std::string& out, const ir::Inlines& inlines, const InlineMarkup& markup,
                         const ir::Document* document);

void wrap_pair(std::string& out, const std::array<std::string, 2>& pair, const ir::Inlines& children,
               const InlineMarkup& markup, const ir::Document* document) {
  out.append(pair[0]);
  render_inlines_into(out, children, markup, document);
  out.append(pair[1]);
}

void render_inlines_into(std::string& out, const ir::Inlines& inlines, const InlineMarkup& markup,
                         const ir::Document* document) {
  for (const ir::Inline& item : inlines) {
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Text>) {
            std::string text = markup.escape ? markup.escape(node.text) : node.text;
            if (markup.soft_break != "\n") text = util::replace_all(text, "\n", markup.soft_break);
            out.append(text);
          } else if constexpr (std::same_as<T, ir::Emphasis>) {
            wrap_pair(out, markup.emphasis, node.children, markup, document);
          } else if constexpr (std::same_as<T, ir::Strong>) {
            if (item.attr.has_class("mark") && !markup.mark[0].empty()) {
              wrap_pair(out, markup.mark, node.children, markup, document);
            } else {
              wrap_pair(out, markup.strong, node.children, markup, document);
            }
          } else if constexpr (std::same_as<T, ir::Code>) {
            if (markup.code) {
              out.append(markup.code(node));
            } else {
              out.append(markup.escape ? markup.escape(node.text) : node.text);
            }
          } else if constexpr (std::same_as<T, ir::Link>) {
            if (!node.note.empty() && markup.note) {
              const std::size_t index = document != nullptr ? footnote_index(*document, node.note) : 0;
              out.append(markup.note(node, index));
            } else {
              std::string label;
              render_inlines_into(label, node.children, markup, document);
              if (markup.link) {
                out.append(markup.link(node, label));
              } else {
                out.append(label);
              }
            }
          } else if constexpr (std::same_as<T, ir::Strikethrough>) {
            wrap_pair(out, markup.strikethrough, node.children, markup, document);
          } else if constexpr (std::same_as<T, ir::Underline>) {
            wrap_pair(out, markup.underline, node.children, markup, document);
          } else if constexpr (std::same_as<T, ir::Superscript>) {
            wrap_pair(out, markup.superscript, node.children, markup, document);
          } else if constexpr (std::same_as<T, ir::Subscript>) {
            wrap_pair(out, markup.subscript, node.children, markup, document);
          } else if constexpr (std::same_as<T, ir::LineBreak>) {
            out.append(markup.line_break);
          } else if constexpr (std::same_as<T, ir::InlineImage>) {
            if (markup.image) {
              out.append(markup.image(node));
            } else {
              out.append(markup.escape ? markup.escape(node.alt) : node.alt);
            }
          }
        },
        item.node);
  }
}

}  // namespace

std::string render_inlines(const ir::Inlines& inlines, const InlineMarkup& markup, const ir::Document* document) {
  std::string out;
  render_inlines_into(out, inlines, markup, document);
  return out;
}

// =========================================================================== inline parsing

void append_text(ir::Inlines& out, std::string_view text) {
  if (text.empty()) return;
  if (!out.empty()) {
    if (auto* last = out.back().get_if<ir::Text>(); last != nullptr && out.back().attr.empty()) {
      last->text.append(text);
      return;
    }
  }
  out.push_back(ir::Text{std::string(text)});
}

namespace {

constexpr int max_inline_parse_depth = 24;

bool is_word_char(std::string_view text, std::size_t position) {
  if (position >= text.size()) return false;
  const auto c = static_cast<unsigned char>(text[position]);
  return c >= 0x80 || util::is_ascii_alpha(static_cast<char>(c)) || util::is_ascii_digit(static_cast<char>(c));
}

bool is_space_at(std::string_view text, std::size_t position) {
  return position >= text.size() || util::is_ascii_space(text[position]);
}

ir::Inline make_node(InlineKind kind, ir::Inlines children, std::string_view raw) {
  switch (kind) {
    case InlineKind::Strong: return ir::Strong{std::move(children)};
    case InlineKind::Emphasis: return ir::Emphasis{std::move(children)};
    case InlineKind::Strikethrough: return ir::Strikethrough{std::move(children)};
    case InlineKind::Underline: return ir::Underline{std::move(children)};
    case InlineKind::Superscript: return ir::Superscript{std::move(children)};
    case InlineKind::Subscript: return ir::Subscript{std::move(children)};
    case InlineKind::Code: return ir::Code{std::string(raw)};
    case InlineKind::Math: {
      ir::Code code{std::string(raw)};
      code.math = ir::MathMode::Inline;
      return code;
    }
    case InlineKind::Mark: {
      ir::Attributes attr;
      attr.add_class("mark");
      return ir::Inline(ir::Strong{std::move(children)}, std::move(attr));
    }
    case InlineKind::Span: return ir::Emphasis{std::move(children)};
  }
  return ir::Text{std::string(raw)};
}

}  // namespace

ir::Inlines InlineParser::parse(std::string_view text, int depth) {
  ir::Inlines out;
  const int saved_depth = depth_;
  depth_ = depth;
  // For each rule, the position from which no closer exists anymore.
  std::vector<std::size_t> no_close_from(syntax_.rules.size(), std::string_view::npos);
  std::size_t literal_start = 0;
  std::size_t i = 0;
  const auto flush = [&](std::size_t end) {
    if (end > literal_start) {
      std::string_view piece = text.substr(literal_start, end - literal_start);
      if (syntax_.newline_is_break) {
        std::size_t start = 0;
        for (std::size_t nl = piece.find('\n'); nl != std::string_view::npos; nl = piece.find('\n', start)) {
          append_text(out, piece.substr(start, nl - start));
          out.push_back(ir::LineBreak{});
          start = nl + 1;
        }
        append_text(out, piece.substr(start));
      } else {
        append_text(out, piece);
      }
    }
  };
  while (i < text.size()) {
    if (syntax_.hook) {
      ir::Inlines produced;
      const std::size_t consumed = syntax_.hook(*this, text, i, produced);
      if (consumed > 0) {
        flush(i);
        for (ir::Inline& node : produced) {
          if (const auto* t = node.get_if<ir::Text>(); t != nullptr && node.attr.empty()) {
            append_text(out, t->text);
          } else {
            out.push_back(std::move(node));
          }
        }
        i += consumed;
        literal_start = i;
        continue;
      }
    }
    if (syntax_.escape != 0 && text[i] == syntax_.escape && i + 1 < text.size()) {
      const char next = text[i + 1];
      if (!util::is_ascii_alpha(next) && !util::is_ascii_digit(next) && !util::is_ascii_space(next)) {
        flush(i);
        append_text(out, text.substr(i + 1, 1));
        i += 2;
        literal_start = i;
        continue;
      }
    }
    bool matched = false;
    if (depth_ < max_inline_parse_depth) {
      for (std::size_t r = 0; r < syntax_.rules.size() && !matched; ++r) {
        const InlineRule& rule = syntax_.rules[r];
        if (!text.substr(i).starts_with(rule.open)) continue;
        if (no_close_from[r] != std::string_view::npos && i >= no_close_from[r]) continue;
        const std::size_t content_start = i + rule.open.size();
        if (rule.flanking) {
          if (is_space_at(text, content_start)) continue;
          if (!rule.intraword && i > 0 && is_word_char(text, i - 1)) continue;
        }
        // Find the closer.
        std::size_t search = content_start;
        std::size_t close = std::string_view::npos;
        while (search <= text.size()) {
          const std::size_t found = text.find(rule.close, search);
          if (found == std::string_view::npos) break;
          bool ok = found > content_start;
          if (ok && rule.flanking) {
            if (util::is_ascii_space(text[found - 1])) ok = false;
            if (!rule.intraword && is_word_char(text, found + rule.close.size())) ok = false;
          }
          if (ok) {
            close = found;
            break;
          }
          search = found + 1;
        }
        if (close == std::string_view::npos) {
          no_close_from[r] = i;
          continue;
        }
        flush(i);
        const std::string_view inner = text.substr(content_start, close - content_start);
        if (rule.verbatim) {
          out.push_back(make_node(rule.kind, {}, inner));
        } else {
          ir::Inlines children = parse(inner, depth + 1);
          depth_ = depth;
          out.push_back(make_node(rule.kind, std::move(children), inner));
        }
        i = close + rule.close.size();
        literal_start = i;
        matched = true;
      }
    }
    if (!matched) ++i;
  }
  flush(text.size());
  depth_ = saved_depth;
  return out;
}

// =========================================================================== lines

std::vector<std::string_view> lines_of(std::string_view text) { return util::split_lines(text); }

std::size_t indentation(std::string_view line) {
  std::size_t width = 0;
  for (const char c : line) {
    if (c == ' ') {
      ++width;
    } else if (c == '\t') {
      width += 4 - width % 4;
    } else {
      break;
    }
  }
  return width;
}

bool is_blank(std::string_view line) { return util::trim(line).empty(); }

std::string_view strip_indent(std::string_view line, std::size_t columns) {
  std::size_t width = 0;
  std::size_t i = 0;
  while (i < line.size() && width < columns) {
    if (line[i] == ' ') {
      ++width;
    } else if (line[i] == '\t') {
      const std::size_t next = width + 4 - width % 4;
      if (next > columns) break;
      width = next;
    } else {
      break;
    }
    ++i;
  }
  return line.substr(i);
}

std::string trim_copy(std::string_view text) { return std::string(util::trim(text)); }

bool is_repeated(std::string_view line, char c, std::size_t minimum) {
  const std::string_view t = util::trim(line);
  if (t.size() < minimum) return false;
  return std::ranges::all_of(t, [c](char x) { return x == c; });
}

std::string join_lines(std::span<const std::string_view> lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) out.push_back('\n');
    out.append(lines[i]);
  }
  return out;
}

std::string inline_line(const ir::Inlines& inlines) {
  std::string text = ir::plain_text(inlines);
  for (char& c : text) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return text;
}

std::string lower(std::string_view text) { return util::to_lower(text); }

std::string collapse_whitespace(std::string_view text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (util::is_ascii_space(c)) {
      space = !out.empty();
    } else {
      if (space) out.push_back(' ');
      space = false;
      out.push_back(c);
    }
  }
  return out;
}

std::string language_from_classes(std::string_view classes) {
  const std::vector<std::string_view> parts = util::split(classes, ' ', false);
  for (const std::string_view part : parts) {
    for (const std::string_view prefix : {"language-", "lang-", "highlight-source-", "highlight-"}) {
      if (part.starts_with(prefix) && part.size() > prefix.size()) return std::string(part.substr(prefix.size()));
    }
  }
  for (const std::string_view part : parts) {
    if (part != "sourceCode" && part != "code" && part != "highlight" && part != "numberLines" && part != "src" &&
        part != "source" && part != "literal-block" && part != "listing" && part != "example") {
      if (part.starts_with("src-")) return std::string(part.substr(4));
      return std::string(part);
    }
  }
  return {};
}

// =========================================================================== tables

std::size_t header_row_count(const ir::TableBlock& table) {
  std::size_t count = 0;
  while (count < table.rows.size() && table.rows[count].header) ++count;
  return count;
}

std::vector<std::vector<std::string>> table_strings(const ir::TableBlock& table,
                                                    const std::function<std::string(const ir::Inlines&)>& render) {
  const TableGrid grid = table_grid(table);
  std::vector<std::vector<std::string>> out(grid.cells.size(), std::vector<std::string>(grid.columns));
  for (std::size_t r = 0; r < grid.cells.size(); ++r) {
    for (std::size_t c = 0; c < grid.columns; ++c) {
      if (grid.cells[r][c] != nullptr && grid.origin[r][c]) out[r][c] = render(grid.cells[r][c]->content);
    }
  }
  return out;
}

std::string pad_to(std::string_view text, std::size_t width, ir::Alignment align) {
  const std::size_t have = display_width(text);
  if (have >= width) return std::string(text);
  const std::size_t gap = width - have;
  std::size_t left = 0;
  if (align == ir::Alignment::Right) left = gap;
  if (align == ir::Alignment::Center) left = gap / 2;
  std::string out(left, ' ');
  out.append(text);
  out.append(gap - left, ' ');
  return out;
}

std::string render_grid_table(const std::vector<std::vector<std::string>>& cells, std::size_t header_rows,
                              const std::vector<ir::Alignment>& alignments, const GridTableStyle& style,
                              std::string_view indent) {
  std::size_t columns = 0;
  for (const auto& row : cells) columns = std::max(columns, row.size());
  if (columns == 0) return {};
  std::vector<std::size_t> widths(columns, 1);
  std::vector<std::vector<std::vector<std::string_view>>> split_cells(cells.size());
  for (std::size_t r = 0; r < cells.size(); ++r) {
    split_cells[r].resize(columns);
    for (std::size_t c = 0; c < columns; ++c) {
      if (c < cells[r].size()) split_cells[r][c] = util::split_lines(cells[r][c]);
      for (const std::string_view line : split_cells[r][c]) widths[c] = std::max(widths[c], display_width(line));
    }
  }
  const auto rule = [&](char fill) {
    std::string line(indent);
    line.push_back('+');
    for (std::size_t c = 0; c < columns; ++c) {
      line.append(widths[c] + 2, fill);
      line.push_back('+');
    }
    line.push_back('\n');
    return line;
  };
  std::string out = rule('-');
  for (std::size_t r = 0; r < cells.size(); ++r) {
    std::size_t height = 1;
    for (const auto& parts : split_cells[r]) height = std::max(height, parts.size());
    for (std::size_t line = 0; line < height; ++line) {
      out.append(indent);
      out.push_back('|');
      for (std::size_t c = 0; c < columns; ++c) {
        const std::string_view piece = line < split_cells[r][c].size() ? split_cells[r][c][line] : std::string_view{};
        const ir::Alignment align = c < alignments.size() ? alignments[c] : ir::Alignment::Left;
        out.push_back(' ');
        out.append(pad_to(piece, widths[c], align == ir::Alignment::Default ? ir::Alignment::Left : align));
        out.append(" |");
      }
      out.push_back('\n');
    }
    out.append(rule(header_rows > 0 && r + 1 == header_rows ? style.header_rule : '-'));
  }
  return out;
}

// =========================================================================== PNG

Bytes encode_png(const ir::Frame& frame) {
  if (!frame.valid()) return {};
  Bytes raw;
  raw.reserve(static_cast<std::size_t>(frame.height) * (static_cast<std::size_t>(frame.width) * 4 + 1));
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    raw.push_back(std::byte{0});
    const auto* row = frame.rgba.data() + frame.offset(0, y);
    for (std::size_t i = 0; i < static_cast<std::size_t>(frame.width) * 4; ++i) raw.push_back(static_cast<std::byte>(row[i]));
  }
  auto compressed = util::zlib_compress(raw, 6);
  if (!compressed) return {};
  util::ByteWriter out;
  out.bytes(as_bytes("\x89PNG\r\n\x1A\n"));
  const auto chunk = [&](std::string_view type, ByteView data) {
    out.u32be(static_cast<std::uint32_t>(data.size()));
    Bytes body = to_bytes(type);
    append(body, data);
    out.bytes(body);
    out.u32be(util::crc32(body));
  };
  util::ByteWriter header;
  header.u32be(frame.width).u32be(frame.height).u8(8).u8(6).u8(0).u8(0).u8(0);
  chunk("IHDR", header.view());
  chunk("IDAT", *compressed);
  chunk("IEND", {});
  return out.take();
}

// =========================================================================== traversal

void for_each_block(ir::Blocks& blocks, const std::function<void(ir::Block&)>& visit, int depth) {
  if (depth > ir::max_document_depth * 2) return;
  for (ir::Block& block : blocks) {
    visit(block);
    if (auto* list = block.get_if<ir::List>()) {
      for (ir::Blocks& item : list->items) for_each_block(item, visit, depth + 1);
    } else if (auto* quote = block.get_if<ir::BlockQuote>()) {
      for_each_block(quote->blocks, visit, depth + 1);
    }
  }
}

namespace {

void visit_inlines_deep(ir::Inlines& inlines, const std::function<void(ir::Inlines&)>& visit) { visit(inlines); }

}  // namespace

void for_each_inlines(ir::Document& document, const std::function<void(ir::Inlines&)>& visit) {
  const auto on_block = [&](ir::Block& block) {
    if (auto* p = block.get_if<ir::Paragraph>()) {
      visit_inlines_deep(p->content, visit);
    } else if (auto* h = block.get_if<ir::Heading>()) {
      visit_inlines_deep(h->content, visit);
    } else if (auto* t = block.get_if<ir::TableBlock>()) {
      for (ir::TableRow& row : t->rows) {
        for (ir::TableCell& cell : row.cells) visit_inlines_deep(cell.content, visit);
      }
      visit_inlines_deep(t->caption, visit);
    }
  };
  for_each_block(document.blocks, on_block);
  for (ir::Footnote& note : document.footnotes) for_each_block(note.blocks, on_block);
}

std::string document_title(const ir::Document& document) {
  if (!document.metadata.title.empty()) return document.metadata.title;
  for (const ir::Block& block : document.blocks) {
    if (const auto* h = block.get_if<ir::Heading>(); h != nullptr && h->level == 1) return inline_line(h->content);
  }
  return {};
}

}  // namespace cvt::document
