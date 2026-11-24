// Convertible core - document family: XML document formats: DocBook (read and
// write), FictionBook 2 (read and write), JATS (read), TEI (read), OPML
// (outline <-> nested lists) and Evernote ENEX (read).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>

#include "engine/registry.hpp"
#include "families/document/doc_common.hpp"
#include "families/document/formats.hpp"
#include "families/document/wiki_engine.hpp"
#include "util/base64.hpp"
#include "util/hash.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"
#include "util/xml.hpp"

namespace cvt::document {

namespace {

using util::XmlDocument;
using util::XmlNode;

constexpr int max_depth = 60;

Result<XmlDocument> parse(ByteView data) {
  util::XmlParseOptions options;
  options.preserve_whitespace = true;
  options.replace_invalid_text = true;
  options.keep_unknown_entities = true;
  options.max_depth = 512;
  auto parsed = util::parse_xml(data, options);
  if (!parsed) return fail(Status::Decode, "not a well-formed XML document: {}", parsed.error().message);
  return parsed;
}

std::string attr(XmlNode node, std::string_view name) { return std::string(node.attribute_local(name).value()); }

// Collapse whitespace like a browser: runs become one space, the run is trimmed.
void finish(ir::Inlines& inlines) {
  bool last_space = true;
  const auto walk = [&](auto&& self, ir::Inlines& list, int depth) -> void {
    if (depth > 128) return;
    for (ir::Inline& item : list) {
      if (auto* t = item.get_if<ir::Text>()) {
        std::string out;
        for (const char c : t->text) {
          if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            if (!last_space) out.push_back(' ');
            last_space = true;
          } else {
            out.push_back(c);
            last_space = false;
          }
        }
        t->text = std::move(out);
      } else if (item.is<ir::LineBreak>()) {
        last_space = true;
      } else if (item.is<ir::Code>() || item.is<ir::InlineImage>()) {
        last_space = false;
      } else {
        std::visit([&](auto& node) {
          if constexpr (requires { node.children; }) self(self, node.children, depth + 1);
        }, item.node);
      }
    }
  };
  walk(walk, inlines, 0);
  // Trailing space.
  for (auto it = inlines.rbegin(); it != inlines.rend(); ++it) {
    if (auto* t = it->get_if<ir::Text>()) {
      while (!t->text.empty() && t->text.back() == ' ') t->text.pop_back();
      if (!t->text.empty()) break;
      continue;
    }
    break;
  }
  normalize_inlines(inlines);
}

ir::Inline make_note_ref(const std::string& label) {
  ir::Link link;
  link.note = label;
  link.url = "#fn-" + label;
  link.children = {ir::Text{label}};
  return link;
}

// ============================================================================ generic reader

class XmlReader {
 public:
  explicit XmlReader(ir::Document& document) : doc_(document) {}
  virtual ~XmlReader() = default;
  XmlReader(const XmlReader&) = delete;
  XmlReader& operator=(const XmlReader&) = delete;

  ir::Blocks blocks(XmlNode parent, int depth) {
    ir::Blocks out;
    ir::Inlines pending;
    const auto flush = [&] {
      finish(pending);
      if (pending.empty()) return;
      if (pending.size() == 1) {
        if (auto* image = pending.front().get_if<ir::InlineImage>()) {
          out.push_back(ir::ImageBlock{std::move(image->source), std::move(image->alt), {}, std::move(image->title)});
          pending.clear();
          return;
        }
      }
      out.push_back(ir::Paragraph{std::move(pending)});
      pending.clear();
    };
    for (XmlNode child : parent.children()) {
      if (child.is_text()) {
        append_text(pending, child.value());
        continue;
      }
      if (child.type() == util::XmlNodeType::ProcessingInstruction) {
        if (child.name() == "hr" || child.name() == "hard-pagebreak") {
          flush();
          if (child.name() == "hr") out.push_back(ir::HorizontalRule{});
          else out.push_back(ir::PageBreak{});
        }
        continue;
      }
      if (!child.is_element()) continue;
      if (depth > max_depth) {
        append_text(pending, child.text());
        continue;
      }
      if (is_block(child)) {
        flush();
        block(child, out, depth + 1);
      } else {
        inline_node(child, pending, depth + 1);
      }
    }
    flush();
    return out;
  }

  ir::Inlines inlines(XmlNode parent, int depth) {
    ir::Inlines out;
    children_inline(parent, out, depth);
    finish(out);
    return out;
  }

 protected:
  virtual bool is_block(XmlNode node) = 0;
  virtual void block(XmlNode node, ir::Blocks& out, int depth) = 0;
  // Returns false for elements to treat as transparent.
  virtual bool inline_element(XmlNode node, ir::Inlines& out, int depth) = 0;

  void children_inline(XmlNode parent, ir::Inlines& out, int depth) {
    for (XmlNode child : parent.children()) {
      if (child.is_text()) {
        append_text(out, child.value());
      } else if (child.is_element()) {
        inline_node(child, out, depth + 1);
      }
    }
  }

  void inline_node(XmlNode node, ir::Inlines& out, int depth) {
    if (depth > max_depth) {
      append_text(out, node.text());
      return;
    }
    if (!inline_element(node, out, depth)) children_inline(node, out, depth);
  }

  ir::Inlines kids(XmlNode node, int depth) {
    ir::Inlines out;
    children_inline(node, out, depth + 1);
    return out;
  }

  // Block content inside an inline context (a note body, a list item).
  ir::Blocks body(XmlNode node, int depth) { return blocks(node, depth + 1); }

  std::string add_note(ir::Blocks content) {
    const std::string label = std::to_string(doc_.footnotes.size() + 1);
    doc_.footnotes.push_back(ir::Footnote{label, std::move(content)});
    return label;
  }

  static ir::Alignment align_of(std::string_view value) {
    if (value == "left" || value == "start") return ir::Alignment::Left;
    if (value == "right" || value == "end") return ir::Alignment::Right;
    if (value == "center") return ir::Alignment::Center;
    return ir::Alignment::Default;
  }

  // HTML-like table (JATS, TEI uses its own).
  ir::TableBlock html_table(XmlNode table, int depth) {
    ir::TableBlock out;
    const auto rows = [&](auto&& self, XmlNode parent, bool head) -> void {
      for (XmlNode c : parent.elements()) {
        const std::string_view n = c.local_name();
        if (n == "thead") self(self, c, true);
        else if (n == "tbody" || n == "tfoot") self(self, c, false);
        else if (n == "col") out.alignments.push_back(align_of(attr(c, "align")));
        else if (n == "colgroup") self(self, c, head);
        else if (n == "tr") {
          ir::TableRow row;
          bool all_th = true;
          for (XmlNode cell : c.elements()) {
            if (cell.local_name() != "td" && cell.local_name() != "th") continue;
            if (cell.local_name() != "th") all_th = false;
            ir::TableCell tc;
            tc.content = inlines(cell, depth);
            tc.colspan = static_cast<int>(std::clamp(cell.attribute_local("colspan").as_int(1), 1, 1000));
            tc.rowspan = static_cast<int>(std::clamp(cell.attribute_local("rowspan").as_int(1), 1, 1000));
            tc.align = align_of(attr(cell, "align"));
            row.cells.push_back(std::move(tc));
          }
          row.header = (head || all_th) && (out.rows.empty() || out.rows.back().header);
          if (!row.cells.empty()) out.rows.push_back(std::move(row));
        }
      }
    };
    rows(rows, table, false);
    wiki::infer_column_alignment(out);
    return out;
  }

  ir::Document& doc_;
};

// ============================================================================ DocBook

class DocBookReader final : public XmlReader {
 public:
  using XmlReader::XmlReader;

  void read(XmlNode root) {
    for (const std::string_view info_name : {"info", "articleinfo", "bookinfo"}) {
      if (XmlNode info = root.child_local(info_name)) metadata(info);
    }
    if (doc_.metadata.title.empty()) {
      if (XmlNode title = root.child_local("title")) doc_.metadata.title = collapse_whitespace(title.text());
    }
    doc_.metadata.language = attr(root, "lang");
    level_ = 0;
    doc_.blocks = blocks(root, 0);
  }

 private:
  void metadata(XmlNode info) {
    if (XmlNode t = info.child_local("title")) doc_.metadata.title = collapse_whitespace(t.text());
    std::vector<std::string> authors;
    for (XmlNode a : info.descendants()) {
      if (!a.is_element() || a.local_name() != "author") continue;
      if (XmlNode p = a.child_local("personname")) a = p;
      std::string name;
      for (const std::string_view part : {"firstname", "givenname", "othername", "surname"}) {
        if (XmlNode n = a.child_local(part)) name += (name.empty() ? "" : " ") + collapse_whitespace(n.text());
      }
      if (name.empty()) name = collapse_whitespace(a.text());
      if (!name.empty()) authors.push_back(name);
    }
    doc_.metadata.author = util::join(authors, "; ");
    if (XmlNode d = info.child_local("date")) doc_.metadata.date = collapse_whitespace(d.text());
    if (XmlNode a = info.child_local("abstract")) doc_.metadata.extra["description"] = collapse_whitespace(a.text());
  }

  bool is_block(XmlNode n) override {
    static const std::set<std::string_view> names = {
        "section", "sect1", "sect2", "sect3", "sect4", "sect5", "chapter", "appendix", "simplesect", "preface", "part",
        "para", "simpara", "formalpara", "itemizedlist", "orderedlist", "variablelist", "programlisting", "screen",
        "literallayout", "synopsis", "blockquote", "note", "warning", "tip", "important", "caution", "danger",
        "informaltable", "table", "mediaobject", "figure", "informalfigure", "equation", "informalequation", "bridgehead",
        "title", "info", "articleinfo", "sidebar", "example", "informalexample", "procedure", "simplelist", "abstract",
        "epigraph", "glosslist", "calloutlist", "segmentedlist", "qandaset", "address", "remark"};
    return names.contains(n.local_name());
  }

  void block(XmlNode n, ir::Blocks& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "info" || name == "articleinfo" || name == "remark") return;
    if (name == "title") {
      if (level_ > 0) out.push_back(ir::Heading{std::clamp(level_, 1, 6), inlines(n, depth)});
      return;
    }
    if (name == "section" || name.starts_with("sect") || name == "chapter" || name == "appendix" ||
        name == "simplesect" || name == "preface" || name == "part") {
      ++level_;
      ir::Attributes a;
      a.id = attr(n, "id");
      ir::Blocks inner = blocks(n, depth);
      if (XmlNode info = n.child_local("info"); info && !n.child_local("title")) {
        if (XmlNode t = info.child_local("title")) inner.insert(inner.begin(), ir::Heading{std::clamp(level_, 1, 6), inlines(t, depth)});
      }
      if (!inner.empty() && inner.front().is<ir::Heading>()) inner.front().attr = std::move(a);
      --level_;
      for (ir::Block& b : inner) out.push_back(std::move(b));
      return;
    }
    if (name == "bridgehead") {
      out.push_back(ir::Heading{std::clamp(level_ + 1, 1, 6), inlines(n, depth)});
      return;
    }
    if (name == "para" || name == "simpara") {
      for (ir::Block& b : blocks(n, depth)) out.push_back(std::move(b));
      return;
    }
    if (name == "formalpara") {
      if (XmlNode t = n.child_local("title")) out.push_back(ir::Paragraph{{ir::Strong{inlines(t, depth)}}});
      if (XmlNode p = n.child_local("para")) for (ir::Block& b : blocks(p, depth)) out.push_back(std::move(b));
      return;
    }
    if (name == "itemizedlist" || name == "orderedlist" || name == "procedure" || name == "simplelist") {
      ir::List list;
      list.ordered = name == "orderedlist" || name == "procedure";
      list.start = std::clamp(n.attribute_local("startingnumber").as_int(1), -1000000, 1000000);
      const std::string numeration = attr(n, "numeration");
      if (numeration == "loweralpha") list.numbering = ir::ListNumbering::LowerAlpha;
      if (numeration == "upperalpha") list.numbering = ir::ListNumbering::UpperAlpha;
      if (numeration == "lowerroman") list.numbering = ir::ListNumbering::LowerRoman;
      if (numeration == "upperroman") list.numbering = ir::ListNumbering::UpperRoman;
      list.loose = attr(n, "spacing") == "normal";
      for (XmlNode item : n.elements()) {
        const std::string_view in = item.local_name();
        if (in == "listitem" || in == "step") list.items.push_back(blocks(item, depth));
        else if (in == "member") list.items.push_back({ir::Paragraph{inlines(item, depth)}});
        else if (in == "title") out.push_back(ir::Paragraph{{ir::Strong{inlines(item, depth)}}});
      }
      if (!list.items.empty()) out.push_back(std::move(list));
      return;
    }
    if (name == "variablelist" || name == "glosslist") {
      ir::List list;
      list.definition = true;
      for (XmlNode entry : n.elements()) {
        if (entry.local_name() != "varlistentry" && entry.local_name() != "glossentry") continue;
        ir::Blocks item;
        ir::Inlines term;
        for (XmlNode t : entry.elements()) {
          if (t.local_name() == "term" || t.local_name() == "glossterm") {
            if (!term.empty()) term.push_back(ir::LineBreak{});
            for (ir::Inline& x : inlines(t, depth)) term.push_back(std::move(x));
          }
        }
        item.push_back(ir::Paragraph{std::move(term)});
        for (XmlNode d : entry.elements()) {
          if (d.local_name() == "listitem" || d.local_name() == "glossdef") for (ir::Block& b : blocks(d, depth)) item.push_back(std::move(b));
        }
        list.items.push_back(std::move(item));
      }
      if (!list.items.empty()) out.push_back(std::move(list));
      return;
    }
    if (name == "programlisting" || name == "screen" || name == "literallayout" || name == "synopsis" || name == "address") {
      std::string code = n.text();
      if (code.starts_with("\n")) code.erase(0, 1);
      while (code.ends_with('\n') || code.ends_with(' ')) code.pop_back();
      out.push_back(ir::CodeBlock{attr(n, "language"), std::move(code)});
      return;
    }
    if (name == "blockquote" || name == "epigraph" || name == "sidebar" || name == "abstract") {
      out.push_back(ir::BlockQuote{blocks(n, depth)});
      return;
    }
    if (name == "note" || name == "warning" || name == "tip" || name == "important" || name == "caution" || name == "danger") {
      ir::Attributes a;
      a.add_class(ir::conventions::admonition);
      a.add_class(name);
      ir::Blocks inner = blocks(n, depth);
      // The note's own title would become a heading: make it bold text.
      for (ir::Block& b : inner) {
        if (auto* h = b.get_if<ir::Heading>()) b = ir::Paragraph{{ir::Strong{std::move(h->content)}}};
      }
      out.push_back(ir::Block(ir::BlockQuote{std::move(inner)}, std::move(a)));
      return;
    }
    if (name == "informaltable" || name == "table") {
      out.push_back(table(n, depth));
      return;
    }
    if (name == "mediaobject" || name == "figure" || name == "informalfigure") {
      XmlNode data;
      for (XmlNode d : n.descendants()) {
        if (d.is_element() && d.local_name() == "imagedata") {
          data = d;
          break;
        }
      }
      std::string alt;
      for (XmlNode d : n.descendants()) {
        if (d.is_element() && d.local_name() == "textobject") {
          alt = collapse_whitespace(d.text());
          break;
        }
      }
      std::string caption;
      if (XmlNode t = n.child_local("title")) caption = collapse_whitespace(t.text());
      for (XmlNode d : n.descendants()) {
        if (d.is_element() && d.local_name() == "caption") caption = collapse_whitespace(d.text());
      }
      if (data) {
        out.push_back(ir::ImageBlock{wiki::image_from_url(attr(data, "fileref")), alt.empty() ? caption : alt, caption, {}});
      } else {
        for (ir::Block& b : blocks(n, depth)) out.push_back(std::move(b));
      }
      return;
    }
    if (name == "equation" || name == "informalequation") {
      ir::CodeBlock math{{}, collapse_whitespace(math_text(n))};
      math.math = ir::MathMode::Display;
      out.push_back(std::move(math));
      return;
    }
    // example, calloutlist, qandaset, ...: content.
    for (ir::Block& b : blocks(n, depth)) out.push_back(std::move(b));
  }

  static std::string math_text(XmlNode n) {
    for (XmlNode d : n.descendants()) {
      if (!d.is_element()) continue;
      if (d.local_name() == "mathphrase" || d.local_name() == "alt") return d.text();
      if (d.local_name() == "math" && d.attribute_local("alttext")) return attr(d, "alttext");
    }
    return n.text();
  }

  ir::TableBlock table(XmlNode n, int depth) {
    ir::TableBlock out;
    if (XmlNode t = n.child_local("title")) out.caption = inlines(t, depth);
    if (XmlNode c = n.child_local("caption")) out.caption = inlines(c, depth);
    XmlNode group = n.child_local("tgroup");
    if (!group) {
      // HTML table model inside DocBook.
      ir::TableBlock html = html_table(n, depth);
      html.caption = std::move(out.caption);
      return html;
    }
    std::vector<std::string> colnames;
    for (XmlNode spec : group.children_local("colspec")) {
      out.alignments.push_back(align_of(attr(spec, "align")));
      std::string colname = attr(spec, "colname");
      if (colname.empty()) colname = "c" + std::to_string(colnames.size() + 1);
      colnames.push_back(colname);
    }
    const auto column_index = [&](const std::string& colname) -> int {
      const auto it = std::ranges::find(colnames, colname);
      return it == colnames.end() ? -1 : static_cast<int>(it - colnames.begin());
    };
    for (XmlNode part : group.elements()) {
      const bool head = part.local_name() == "thead";
      if (!head && part.local_name() != "tbody" && part.local_name() != "tfoot") continue;
      for (XmlNode row : part.children_local("row")) {
        ir::TableRow r;
        r.header = head;
        for (XmlNode entry : row.children_local("entry")) {
          ir::TableCell cell;
          // Entries may hold paras.
          ir::Blocks content = blocks(entry, depth);
          for (std::size_t k = 0; k < content.size(); ++k) {
            if (k > 0) cell.content.push_back(ir::LineBreak{});
            if (auto* p = content[k].get_if<ir::Paragraph>()) for (ir::Inline& x : p->content) cell.content.push_back(std::move(x));
            else append_text(cell.content, ir::plain_text(ir::Blocks{content[k]}));
          }
          const int start = column_index(attr(entry, "namest"));
          const int end = column_index(attr(entry, "nameend"));
          if (start >= 0 && end >= start) cell.colspan = std::min(end - start + 1, 1000);
          cell.rowspan = std::clamp(entry.attribute_local("morerows").as_int(0) + 1, 1, 1000);
          cell.align = align_of(attr(entry, "align"));
          r.cells.push_back(std::move(cell));
        }
        out.rows.push_back(std::move(r));
      }
    }
    return out;
  }

  bool inline_element(XmlNode n, ir::Inlines& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "emphasis") {
      const std::string role = attr(n, "role");
      ir::Inlines k = kids(n, depth);
      if (role == "strong" || role == "bold") out.push_back(ir::Strong{std::move(k)});
      else if (role == "strikethrough" || role == "strike") out.push_back(ir::Strikethrough{std::move(k)});
      else if (role == "underline") out.push_back(ir::Underline{std::move(k)});
      else if (role == "highlight" || role == "mark") {
        ir::Attributes a;
        a.add_class("mark");
        out.push_back(ir::Inline(ir::Strong{std::move(k)}, std::move(a)));
      } else out.push_back(ir::Emphasis{std::move(k)});
      return true;
    }
    static const std::set<std::string_view> mono = {"literal", "code", "command", "filename", "option", "varname",
                                                    "function", "userinput", "computeroutput", "classname", "methodname",
                                                    "envar", "systemitem", "constant", "type", "parameter", "keycap",
                                                    "markup", "tag", "uri", "prompt", "replaceable", "property"};
    if (mono.contains(name)) {
      out.push_back(ir::Code{n.text()});
      return true;
    }
    if (name == "link" || name == "ulink" || name == "olink" || name == "xref") {
      std::string url = attr(n, "href");
      if (url.empty()) url = attr(n, "url");
      if (url.empty() && !attr(n, "linkend").empty()) url = "#" + attr(n, "linkend");
      ir::Inlines label = kids(n, depth);
      if (label.empty()) label = {ir::Text{url.starts_with("#") ? url.substr(1) : url}};
      out.push_back(ir::Link{url, {}, std::move(label)});
      return true;
    }
    if (name == "superscript") return out.push_back(ir::Superscript{kids(n, depth)}), true;
    if (name == "subscript") return out.push_back(ir::Subscript{kids(n, depth)}), true;
    if (name == "citetitle" || name == "firstterm" || name == "glossterm" || name == "foreignphrase") {
      return out.push_back(ir::Emphasis{kids(n, depth)}), true;
    }
    if (name == "quote") {
      append_text(out, "\xE2\x80\x9C");
      for (ir::Inline& k : kids(n, depth)) out.push_back(std::move(k));
      append_text(out, "\xE2\x80\x9D");
      return true;
    }
    if (name == "footnote") {
      out.push_back(make_note_ref(add_note(body(n, depth))));
      return true;
    }
    if (name == "inlinemediaobject") {
      for (XmlNode d : n.descendants()) {
        if (d.is_element() && d.local_name() == "imagedata") {
          std::string alt;
          if (XmlNode t = n.child_local("textobject")) alt = collapse_whitespace(t.text());
          out.push_back(ir::InlineImage{wiki::image_from_url(attr(d, "fileref")), alt});
          return true;
        }
      }
      return true;
    }
    if (name == "inlineequation") {
      ir::Code math{collapse_whitespace(math_text(n))};
      math.math = ir::MathMode::Inline;
      out.push_back(std::move(math));
      return true;
    }
    if (name == "anchor" || name == "indexterm" || name == "remark") return true;
    return false;  // phrase, trademark, abbrev, ... transparent
  }

  int level_ = 0;
};

// ============================================================================ JATS

class JatsReader final : public XmlReader {
 public:
  using XmlReader::XmlReader;

  void read(XmlNode root) {
    XmlNode meta = root.find("front/article-meta");
    if (XmlNode t = meta.find("title-group/article-title")) doc_.metadata.title = collapse_whitespace(t.text());
    std::vector<std::string> authors;
    for (XmlNode contrib : meta.find_all("contrib-group/contrib")) {
      XmlNode name = contrib.child_local("name");
      std::string n = collapse_whitespace(name.child_local("given-names").text());
      const std::string surname = collapse_whitespace(name.child_local("surname").text());
      if (!surname.empty()) n += (n.empty() ? "" : " ") + surname;
      if (n.empty()) n = collapse_whitespace(contrib.child_local("string-name").text());
      if (!n.empty()) authors.push_back(n);
    }
    doc_.metadata.author = util::join(authors, "; ");
    if (XmlNode d = meta.child_local("pub-date")) {
      std::string date = collapse_whitespace(d.child_local("year").text());
      if (XmlNode m = d.child_local("month")) date += "-" + collapse_whitespace(m.text());
      if (XmlNode day = d.child_local("day")) date += "-" + collapse_whitespace(day.text());
      doc_.metadata.date = date;
    }
    if (XmlNode a = meta.child_local("abstract")) doc_.metadata.extra["description"] = collapse_whitespace(a.text());
    doc_.metadata.language = attr(root, "lang");
    // Footnotes first, so xrefs resolve.
    for (XmlNode fn : root.descendants()) {
      if (fn.is_element() && fn.local_name() == "fn" && !attr(fn, "id").empty()) note_nodes_[attr(fn, "id")] = fn;
    }
    if (XmlNode body = root.child_local("body")) doc_.blocks = blocks(body, 0);
    if (XmlNode back = root.child_local("back")) {
      for (XmlNode part : back.elements()) {
        if (part.local_name() == "fn-group") continue;
        for (ir::Block& b : blocks(part.parent() == back ? back : part, 0)) (void)b;
        break;
      }
      for (XmlNode refs : back.children_local("ref-list")) {
        ir::Blocks list_blocks = blocks(refs, 0);
        for (ir::Block& b : list_blocks) doc_.blocks.push_back(std::move(b));
      }
    }
    if (!doc_.footnotes.empty()) number_footnotes(doc_);
  }

 private:
  bool is_block(XmlNode n) override {
    static const std::set<std::string_view> names = {"sec", "title", "p", "list", "def-list", "table-wrap", "fig",
                                                     "disp-quote", "preformat", "code", "boxed-text", "disp-formula",
                                                     "statement", "ref-list", "ref", "fn-group", "fn", "app", "ack",
                                                     "verse-group", "graphic", "table", "label", "caption"};
    return names.contains(n.local_name());
  }

  void block(XmlNode n, ir::Blocks& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "fn-group" || name == "fn" || name == "label") return;
    if (name == "title") {
      out.push_back(ir::Heading{std::clamp(level_, 1, 6), inlines(n, depth)});
      return;
    }
    if (name == "sec" || name == "app" || name == "ack" || name == "ref-list") {
      ++level_;
      ir::Blocks inner = blocks(n, depth);
      if (!inner.empty() && inner.front().is<ir::Heading>()) inner.front().attr.id = attr(n, "id");
      --level_;
      for (ir::Block& b : inner) out.push_back(std::move(b));
      return;
    }
    if (name == "p" || name == "statement") {
      for (ir::Block& b : blocks(n, depth)) out.push_back(std::move(b));
      return;
    }
    if (name == "list") {
      ir::List list;
      const std::string type = attr(n, "list-type");
      list.ordered = type == "order" || type == "alpha-lower" || type == "alpha-upper" || type == "roman-lower" || type == "roman-upper";
      if (type == "alpha-lower") list.numbering = ir::ListNumbering::LowerAlpha;
      if (type == "alpha-upper") list.numbering = ir::ListNumbering::UpperAlpha;
      if (type == "roman-lower") list.numbering = ir::ListNumbering::LowerRoman;
      if (type == "roman-upper") list.numbering = ir::ListNumbering::UpperRoman;
      for (XmlNode item : n.children_local("list-item")) list.items.push_back(blocks(item, depth));
      if (!list.items.empty()) out.push_back(std::move(list));
      return;
    }
    if (name == "def-list") {
      ir::List list;
      list.definition = true;
      for (XmlNode item : n.children_local("def-item")) {
        ir::Blocks entry{ir::Paragraph{inlines(item.child_local("term"), depth)}};
        for (XmlNode def : item.children_local("def")) for (ir::Block& b : blocks(def, depth)) entry.push_back(std::move(b));
        list.items.push_back(std::move(entry));
      }
      if (!list.items.empty()) out.push_back(std::move(list));
      return;
    }
    if (name == "table-wrap" || name == "table") {
      XmlNode table = name == "table" ? n : n.child_local("table");
      ir::TableBlock t = html_table(table, depth);
      if (XmlNode c = n.child_local("caption")) t.caption = inlines(c, depth);
      if (!t.rows.empty()) out.push_back(std::move(t));
      return;
    }
    if (name == "fig" || name == "graphic") {
      XmlNode graphic = name == "graphic" ? n : n.child_local("graphic");
      std::string caption;
      if (XmlNode c = n.child_local("caption")) caption = collapse_whitespace(c.text());
      std::string alt = collapse_whitespace(n.child_local("alt-text").text());
      if (alt.empty()) alt = caption;
      if (graphic) out.push_back(ir::ImageBlock{wiki::image_from_url(attr(graphic, "href")), alt, caption, {}});
      return;
    }
    if (name == "disp-quote" || name == "boxed-text" || name == "verse-group") {
      out.push_back(ir::BlockQuote{blocks(n, depth)});
      return;
    }
    if (name == "preformat" || name == "code") {
      std::string code = n.text();
      if (code.starts_with("\n")) code.erase(0, 1);
      while (code.ends_with('\n')) code.pop_back();
      std::string language = attr(n, "language");
      if (language.empty()) language = attr(n, "code-type");
      out.push_back(ir::CodeBlock{language, code});
      return;
    }
    if (name == "disp-formula") {
      XmlNode tex = n.child_local("tex-math");
      ir::CodeBlock math{{}, collapse_whitespace(tex ? tex.text() : n.text())};
      math.math = ir::MathMode::Display;
      out.push_back(std::move(math));
      return;
    }
    if (name == "ref") {
      out.push_back(ir::Paragraph{inlines(n, depth)});
      return;
    }
    for (ir::Block& b : blocks(n, depth)) out.push_back(std::move(b));
  }

  bool inline_element(XmlNode n, ir::Inlines& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "bold") return out.push_back(ir::Strong{kids(n, depth)}), true;
    if (name == "italic") return out.push_back(ir::Emphasis{kids(n, depth)}), true;
    if (name == "monospace") return out.push_back(ir::Code{n.text()}), true;
    if (name == "underline") return out.push_back(ir::Underline{kids(n, depth)}), true;
    if (name == "strike") return out.push_back(ir::Strikethrough{kids(n, depth)}), true;
    if (name == "sup") return out.push_back(ir::Superscript{kids(n, depth)}), true;
    if (name == "sub") return out.push_back(ir::Subscript{kids(n, depth)}), true;
    if (name == "ext-link" || name == "uri") {
      std::string url = attr(n, "href");
      if (url.empty()) url = collapse_whitespace(n.text());
      ir::Inlines label = kids(n, depth);
      if (label.empty()) label = {ir::Text{url}};
      out.push_back(ir::Link{url, {}, std::move(label)});
      return true;
    }
    if (name == "xref") {
      const std::string rid = attr(n, "rid");
      if (attr(n, "ref-type") == "fn" || note_nodes_.contains(rid)) {
        const auto it = note_nodes_.find(rid);
        if (it != note_nodes_.end()) {
          if (!used_notes_.contains(rid)) used_notes_[rid] = add_note(body(it->second, depth));
          out.push_back(make_note_ref(used_notes_[rid]));
          return true;
        }
      }
      ir::Inlines label = kids(n, depth);
      if (label.empty()) label = {ir::Text{rid}};
      out.push_back(ir::Link{"#" + rid, {}, std::move(label)});
      return true;
    }
    if (name == "inline-formula") {
      XmlNode tex = n.child_local("tex-math");
      ir::Code math{collapse_whitespace(tex ? tex.text() : n.text())};
      math.math = ir::MathMode::Inline;
      out.push_back(std::move(math));
      return true;
    }
    if (name == "inline-graphic") return out.push_back(ir::InlineImage{wiki::image_from_url(attr(n, "href")), {}}), true;
    if (name == "fn") {
      out.push_back(make_note_ref(add_note(body(n, depth))));
      return true;
    }
    if (name == "break") return out.push_back(ir::LineBreak{}), true;
    if (name == "label" && n.parent().local_name() == "fn") return true;
    return false;
  }

  int level_ = 0;
  std::map<std::string, XmlNode> note_nodes_;
  std::map<std::string, std::string> used_notes_;
};

// ============================================================================ TEI

class TeiReader final : public XmlReader {
 public:
  using XmlReader::XmlReader;

  void read(XmlNode root) {
    XmlNode title_stmt = root.find("teiHeader/fileDesc/titleStmt");
    if (XmlNode t = title_stmt.child_local("title")) doc_.metadata.title = collapse_whitespace(t.text());
    std::vector<std::string> authors;
    for (XmlNode a : title_stmt.children_local("author")) authors.push_back(collapse_whitespace(a.text()));
    doc_.metadata.author = util::join(authors, "; ");
    XmlNode body = root.find("text/body");
    if (!body) body = root.child_local("text");
    for (const std::string_view part : {"front", "body", "back"}) {
      XmlNode p = root.child_local("text").child_local(part);
      if (!p && part == "body") p = body;
      if (p) for (ir::Block& b : blocks(p, 0)) doc_.blocks.push_back(std::move(b));
    }
    if (!doc_.footnotes.empty()) number_footnotes(doc_);
  }

 private:
  bool is_block(XmlNode n) override {
    static const std::set<std::string_view> names = {"div", "div1", "div2", "div3", "div4", "head", "p", "ab", "list",
                                                     "table", "quote", "cit", "figure", "eg", "lg", "sp", "listBibl",
                                                     "divGen", "trailer", "byline", "docTitle", "titlePage", "opener", "closer", "milestone"};
    if (n.local_name() == "ab") return attr(n, "type").starts_with("codeblock") || n.parent().local_name() != "p";
    if (n.local_name() == "quote") return n.parent().local_name() != "p";
    if (n.local_name() == "figure") return n.parent().local_name() != "p";
    return names.contains(n.local_name());
  }

  void block(XmlNode n, ir::Blocks& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "head") {
      if (n.parent().local_name() == "list" || n.parent().local_name() == "table" || n.parent().local_name() == "figure") return;
      out.push_back(ir::Heading{std::clamp(level_, 1, 6), inlines(n, depth)});
      return;
    }
    if (name.starts_with("div")) {
      ++level_;
      ir::Blocks inner = blocks(n, depth);
      if (!inner.empty() && inner.front().is<ir::Heading>()) inner.front().attr.id = attr(n, "id");
      --level_;
      for (ir::Block& b : inner) out.push_back(std::move(b));
      return;
    }
    if (name == "list" || name == "listBibl") {
      ir::List list;
      const std::string type = attr(n, "type") + attr(n, "rend");
      list.ordered = type.starts_with("ordered") || type.find("numbered") != std::string::npos;
      list.definition = type.find("gloss") != std::string::npos || type.find("definition") != std::string::npos;
      std::string pending_label;
      for (XmlNode item : n.elements()) {
        if (item.local_name() == "label") {
          list.definition = true;
          pending_label = item.text();
          list.items.push_back({ir::Paragraph{inlines(item, depth)}});
          continue;
        }
        if (item.local_name() != "item" && item.local_name() != "bibl") continue;
        ir::Blocks content = blocks(item, depth);
        if (list.definition && !pending_label.empty() && !list.items.empty()) {
          for (ir::Block& b : content) list.items.back().push_back(std::move(b));
          pending_label.clear();
        } else {
          if (list.definition) content.insert(content.begin(), ir::Paragraph{});
          list.items.push_back(std::move(content));
        }
      }
      if (!list.items.empty()) out.push_back(std::move(list));
      return;
    }
    if (name == "table") {
      ir::TableBlock t;
      if (XmlNode h = n.child_local("head")) t.caption = inlines(h, depth);
      for (XmlNode row : n.children_local("row")) {
        ir::TableRow r;
        r.header = attr(row, "role") == "label" && (t.rows.empty() || t.rows.back().header);
        for (XmlNode cell : row.children_local("cell")) {
          ir::TableCell c;
          c.content = inlines(cell, depth);
          c.colspan = std::clamp(cell.attribute_local("cols").as_int(1), 1, 1000);
          c.rowspan = std::clamp(cell.attribute_local("rows").as_int(1), 1, 1000);
          if (attr(cell, "role") == "label" && t.rows.empty()) r.header = true;
          r.cells.push_back(std::move(c));
        }
        if (!r.cells.empty()) t.rows.push_back(std::move(r));
      }
      if (!t.rows.empty()) out.push_back(std::move(t));
      return;
    }
    if (name == "quote" || name == "cit" || name == "lg" || name == "sp") {
      if (name == "lg") {
        ir::Inlines lines;
        for (XmlNode l : n.children_local("l")) {
          if (!lines.empty()) lines.push_back(ir::LineBreak{});
          for (ir::Inline& x : inlines(l, depth)) lines.push_back(std::move(x));
        }
        ir::Attributes a;
        a.add_class(ir::conventions::line_block);
        out.push_back(ir::BlockQuote{{ir::Block(ir::Paragraph{std::move(lines)}, std::move(a))}});
        return;
      }
      out.push_back(ir::BlockQuote{blocks(n, depth)});
      return;
    }
    if (name == "figure") {
      XmlNode graphic = n.child_local("graphic");
      std::string caption = collapse_whitespace(n.child_local("head").text());
      std::string alt = collapse_whitespace(n.child_local("figDesc").text());
      if (graphic) {
        out.push_back(ir::ImageBlock{wiki::image_from_url(attr(graphic, "url")), alt.empty() ? caption : alt, caption, {}});
      }
      return;
    }
    if (name == "ab" && attr(n, "type").starts_with("codeblock")) {
      const std::string type = attr(n, "type");
      std::string code = n.text();
      if (code.starts_with("\n")) code.erase(0, 1);
      while (code.ends_with('\n') || code.ends_with(' ')) code.pop_back();
      // Remove the indentation the XML pretty printer may not have added (none expected).
      out.push_back(ir::CodeBlock{type.size() > 10 ? type.substr(10) : std::string(), code});
      return;
    }
    if (name == "milestone") {
      if (attr(n, "type") == "separator") out.push_back(ir::HorizontalRule{});
      return;
    }
    if (name == "eg") {
      out.push_back(ir::CodeBlock{attr(n, "lang"), n.text()});
      return;
    }
    if (name == "divGen") return;
    for (ir::Block& b : blocks(n, depth)) out.push_back(std::move(b));
  }

  bool inline_element(XmlNode n, ir::Inlines& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "hi" || name == "emph" || name == "term" || name == "title" || name == "foreign" || name == "mentioned") {
      const std::string rend = attr(n, "rend") + attr(n, "rendition");
      ir::Inlines k = kids(n, depth);
      if (rend.find("bold") != std::string::npos || rend.find("strong") != std::string::npos) out.push_back(ir::Strong{std::move(k)});
      else if (rend.find("strike") != std::string::npos) out.push_back(ir::Strikethrough{std::move(k)});
      else if (rend.find("underline") != std::string::npos) out.push_back(ir::Underline{std::move(k)});
      else if (rend.find("sup") != std::string::npos) out.push_back(ir::Superscript{std::move(k)});
      else if (rend.find("sub") != std::string::npos) out.push_back(ir::Subscript{std::move(k)});
      else if (rend.find("code") != std::string::npos || rend.find("mono") != std::string::npos) out.push_back(ir::Code{n.text()});
      else if (name == "hi" && rend.empty()) out.push_back(ir::Strong{std::move(k)});
      else out.push_back(ir::Emphasis{std::move(k)});
      return true;
    }
    if (name == "code" || name == "ident" || name == "gi" || name == "att" || name == "val" ||
        (name == "seg" && attr(n, "type") == "code")) {
      return out.push_back(ir::Code{n.text()}), true;
    }
    if (name == "figure") {
      XmlNode graphic = n.child_local("graphic");
      std::string alt = collapse_whitespace(n.child_local("figDesc").text());
      if (alt.empty()) alt = collapse_whitespace(n.child_local("head").text());
      if (graphic) out.push_back(ir::InlineImage{wiki::image_from_url(attr(graphic, "url")), alt});
      return true;
    }
    if (name == "ref" || name == "ptr") {
      const std::string target = attr(n, "target");
      ir::Inlines label = kids(n, depth);
      if (label.empty()) label = {ir::Text{target}};
      out.push_back(ir::Link{target, {}, std::move(label)});
      return true;
    }
    if (name == "note") {
      out.push_back(make_note_ref(add_note(body(n, depth))));
      return true;
    }
    if (name == "lb") return out.push_back(ir::LineBreak{}), true;
    if (name == "graphic") return out.push_back(ir::InlineImage{wiki::image_from_url(attr(n, "url")), {}}), true;
    if (name == "formula") {
      ir::Code math{collapse_whitespace(n.text())};
      math.math = ir::MathMode::Inline;
      out.push_back(std::move(math));
      return true;
    }
    if (name == "q" || name == "quote" || name == "soCalled") {
      append_text(out, "\xE2\x80\x9C");
      for (ir::Inline& k : kids(n, depth)) out.push_back(std::move(k));
      append_text(out, "\xE2\x80\x9D");
      return true;
    }
    if (name == "pb" || name == "index" || name == "anchor" || name == "milestone") return true;
    return false;
  }

  int level_ = 0;
};

// ============================================================================ FictionBook

class Fb2Reader final : public XmlReader {
 public:
  using XmlReader::XmlReader;

  Result<void> read(XmlNode root, const Context& ctx) {
    XmlNode info = root.find("description/title-info");
    doc_.metadata.title = collapse_whitespace(info.child_local("book-title").text());
    std::vector<std::string> authors;
    for (XmlNode a : info.children_local("author")) {
      std::string name;
      for (const std::string_view part : {"first-name", "middle-name", "last-name"}) {
        const std::string p = collapse_whitespace(a.child_local(part).text());
        if (!p.empty()) name += (name.empty() ? "" : " ") + p;
      }
      if (name.empty()) name = collapse_whitespace(a.child_local("nickname").text());
      if (!name.empty()) authors.push_back(name);
    }
    doc_.metadata.author = util::join(authors, "; ");
    doc_.metadata.language = collapse_whitespace(info.child_local("lang").text());
    doc_.metadata.date = collapse_whitespace(info.child_local("date").text());
    if (XmlNode a = info.child_local("annotation")) doc_.metadata.extra["description"] = collapse_whitespace(a.text());
    if (XmlNode k = info.child_local("keywords")) doc_.metadata.extra["keywords"] = collapse_whitespace(k.text());
    // Binaries.
    std::uint64_t total = 0;
    for (XmlNode binary : root.children_local("binary")) {
      const std::string text = binary.text();
      total += text.size();
      CVT_TRY(ctx.limits.check_decompressed(total));
      std::string cleaned;
      for (const char c : text) {
        if (!util::is_ascii_space(c)) cleaned.push_back(c);
      }
      auto bytes = util::base64_decode(cleaned);
      if (!bytes) continue;
      ir::EncodedImage image;
      image.bytes = std::move(*bytes);
      image.mime = attr(binary, "content-type");
      binaries_[attr(binary, "id")] = std::move(image);
    }
    // Note bodies.
    for (XmlNode body : root.children_local("body")) {
      if (attr(body, "name") != "notes" && attr(body, "name") != "comments") continue;
      for (XmlNode section : body.descendants()) {
        if (section.is_element() && section.local_name() == "section" && !attr(section, "id").empty()) {
          note_nodes_[attr(section, "id")] = section;
        }
      }
    }
    for (XmlNode body : root.children_local("body")) {
      if (!attr(body, "name").empty()) continue;
      level_ = 0;
      if (XmlNode t = body.child_local("title")) {
        // The body title usually repeats the book title.
        const std::string text = collapse_whitespace(t.text());
        if (text != doc_.metadata.title) doc_.blocks.push_back(ir::Heading{1, title_inlines(t, 0)});
      }
      for (XmlNode child : body.elements()) {
        if (child.local_name() == "title") continue;
        ir::Blocks part;
        if (is_block(child)) block(child, part, 1);
        for (ir::Block& b : part) doc_.blocks.push_back(std::move(b));
      }
    }
    if (!doc_.footnotes.empty()) number_footnotes(doc_);
    return {};
  }

 private:
  ir::Inlines title_inlines(XmlNode title, int depth) {
    ir::Inlines out;
    for (XmlNode p : title.children_local("p")) {
      if (!out.empty()) append_text(out, " ");
      for (ir::Inline& x : inlines(p, depth)) out.push_back(std::move(x));
    }
    if (out.empty()) out = inlines(title, depth);
    return out;
  }

  std::optional<ir::EncodedImage> image(XmlNode n) {
    std::string href = attr(n, "href");
    if (href.starts_with("#")) {
      const auto it = binaries_.find(href.substr(1));
      if (it != binaries_.end()) return it->second;
    }
    if (href.empty()) return std::nullopt;
    ir::EncodedImage reference;
    reference.url = href;
    return reference;
  }

  bool is_block(XmlNode n) override {
    static const std::set<std::string_view> names = {"section", "title", "subtitle", "p", "empty-line", "poem", "cite",
                                                     "table", "image", "epigraph", "annotation", "text-author", "stanza", "v"};
    if (n.local_name() == "image") return n.parent().local_name() != "p";
    return names.contains(n.local_name());
  }

  void block(XmlNode n, ir::Blocks& out, int depth) override {
    const std::string_view name = n.local_name();
    if (depth > max_depth) return;
    if (name == "section") {
      ++level_;
      ir::Attributes a;
      a.id = attr(n, "id");
      for (XmlNode child : n.elements()) {
        if (child.local_name() == "title") {
          out.push_back(ir::Block(ir::Heading{std::clamp(level_, 1, 6), title_inlines(child, depth)}, a));
          continue;
        }
        if (is_block(child)) block(child, out, depth + 1);
      }
      --level_;
      return;
    }
    if (name == "title") {
      out.push_back(ir::Heading{std::clamp(level_ + 1, 1, 6), title_inlines(n, depth)});
      return;
    }
    if (name == "subtitle") {
      out.push_back(ir::Heading{std::clamp(level_ + 1, 1, 6), inlines(n, depth)});
      return;
    }
    if (name == "p" || name == "v" || name == "text-author") {
      ir::Inlines content = inlines(n, depth);
      // Monospace-only paragraphs are code lines; consecutive ones form a block.
      if (content.size() == 1 && content.front().is<ir::Code>()) {
        const std::string line = content.front().get_if<ir::Code>()->text;
        if (!out.empty()) {
          if (auto* code = out.back().get_if<ir::CodeBlock>(); code != nullptr && !in_code_break_) {
            code->code += "\n" + line;
            return;
          }
        }
        out.push_back(ir::CodeBlock{{}, line});
        in_code_break_ = false;
        return;
      }
      in_code_break_ = true;
      if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
        auto* img = content.front().get_if<ir::InlineImage>();
        out.push_back(ir::ImageBlock{std::move(img->source), std::move(img->alt), {}, {}});
        return;
      }
      if (!content.empty()) out.push_back(ir::Paragraph{std::move(content)});
      return;
    }
    in_code_break_ = true;
    if (name == "empty-line") return;
    if (name == "image") {
      if (auto img = image(n)) out.push_back(ir::ImageBlock{std::move(*img), attr(n, "alt"), attr(n, "title"), {}});
      return;
    }
    if (name == "cite" || name == "epigraph" || name == "annotation" || name == "poem" || name == "stanza") {
      ir::Blocks inner;
      for (XmlNode child : n.elements()) {
        if (is_block(child)) block(child, inner, depth + 1);
      }
      if (name == "stanza") {
        for (ir::Block& b : inner) out.push_back(std::move(b));
      } else {
        out.push_back(ir::BlockQuote{std::move(inner)});
      }
      return;
    }
    if (name == "table") {
      ir::TableBlock t;
      for (XmlNode tr : n.children_local("tr")) {
        ir::TableRow row;
        bool all_th = true;
        for (XmlNode cell : tr.elements()) {
          if (cell.local_name() != "td" && cell.local_name() != "th") continue;
          if (cell.local_name() != "th") all_th = false;
          ir::TableCell c;
          c.content = inlines(cell, depth);
          c.colspan = std::clamp(cell.attribute_local("colspan").as_int(1), 1, 1000);
          c.rowspan = std::clamp(cell.attribute_local("rowspan").as_int(1), 1, 1000);
          c.align = align_of(attr(cell, "align"));
          row.cells.push_back(std::move(c));
        }
        row.header = all_th && (t.rows.empty() || t.rows.back().header);
        if (!row.cells.empty()) t.rows.push_back(std::move(row));
      }
      wiki::infer_column_alignment(t);
      if (!t.rows.empty()) out.push_back(std::move(t));
    }
  }

  bool inline_element(XmlNode n, ir::Inlines& out, int depth) override {
    const std::string_view name = n.local_name();
    if (name == "strong") return out.push_back(ir::Strong{kids(n, depth)}), true;
    if (name == "emphasis") return out.push_back(ir::Emphasis{kids(n, depth)}), true;
    if (name == "strikethrough") return out.push_back(ir::Strikethrough{kids(n, depth)}), true;
    if (name == "sup") return out.push_back(ir::Superscript{kids(n, depth)}), true;
    if (name == "sub") return out.push_back(ir::Subscript{kids(n, depth)}), true;
    if (name == "code") return out.push_back(ir::Code{n.text()}), true;
    if (name == "a") {
      const std::string href = attr(n, "href");
      if (href.starts_with("#") && (attr(n, "type") == "note" || note_nodes_.contains(href.substr(1)))) {
        const auto it = note_nodes_.find(href.substr(1));
        if (it != note_nodes_.end()) {
          ir::Blocks content;
          for (XmlNode child : it->second.elements()) {
            if (child.local_name() != "title" && is_block(child)) block(child, content, depth + 1);
          }
          out.push_back(make_note_ref(add_note(std::move(content))));
          return true;
        }
      }
      out.push_back(ir::Link{href, {}, kids(n, depth)});
      return true;
    }
    if (name == "image") {
      if (auto img = image(n)) out.push_back(ir::InlineImage{std::move(*img), attr(n, "alt")});
      return true;
    }
    return false;
  }

  std::map<std::string, ir::EncodedImage> binaries_;
  std::map<std::string, XmlNode> note_nodes_;
  int level_ = 0;
  bool in_code_break_ = true;
};

// ============================================================================ OPML

void opml_outline(XmlNode parent, ir::List& list, int depth) {
  for (XmlNode outline : parent.children_local("outline")) {
    std::string text = attr(outline, "text");
    if (text.empty()) text = attr(outline, "title");
    std::string url = attr(outline, "url");
    if (url.empty()) url = attr(outline, "htmlUrl");
    if (url.empty()) url = attr(outline, "xmlUrl");
    ir::Inlines content;
    if (!url.empty()) {
      content.push_back(ir::Link{url, {}, {ir::Text{text.empty() ? url : text}}});
    } else {
      // Outline text may hold simple HTML markup.
      if (text.find('<') != std::string::npos) {
        if (auto parsed = parse_html_text(text); parsed && parsed->blocks.size() == 1 && parsed->blocks[0].is<ir::Paragraph>()) {
          content = std::move(parsed->blocks[0].get_if<ir::Paragraph>()->content);
        }
      }
      if (content.empty()) append_text(content, text);
    }
    ir::Blocks item{ir::Paragraph{std::move(content)}};
    const std::string note = attr(outline, "_note");
    if (!note.empty() && depth < 16) {
      // Notes hold Markdown text.
      if (auto parsed = parse_markdown(note)) {
        for (ir::Block& b : parsed->blocks) item.push_back(std::move(b));
      }
    }
    if (depth < max_depth && outline.child_local("outline")) {
      ir::List nested;
      opml_outline(outline, nested, depth + 1);
      if (!nested.items.empty()) item.push_back(std::move(nested));
    }
    list.items.push_back(std::move(item));
  }
}

Result<ir::Document> read_opml(ByteView data, ReadContext&) {
  CVT_TRY_ASSIGN(XmlDocument xml, parse(data));
  XmlNode root = xml.root();
  if (root.local_name() != "opml") return fail(Status::Decode, "not an OPML document");
  ir::Document document;
  document.metadata.title = collapse_whitespace(root.find("head/title").text());
  document.metadata.author = collapse_whitespace(root.find("head/ownerName").text());
  ir::List list;
  opml_outline(root.child_local("body"), list, 0);
  if (!list.items.empty()) document.blocks.push_back(std::move(list));
  return document;
}

// Headings become nested outlines; paragraphs and list items become outlines below them.
class OpmlWriter {
 public:
  explicit OpmlWriter(const ir::Document& document) : doc_(document) {}

  std::string run() {
    XmlDocument xml;
    xml.set_declaration();
    XmlNode opml = xml.append_child("opml");
    opml.set_attribute("version", "2.0");
    XmlNode head = opml.append_child("head");
    head.append_child("title").set_text(document_title(doc_));
    XmlNode body = opml.append_child("body");
    std::vector<std::pair<int, XmlNode>> stack{{0, body}};
    for (const ir::Block& b : doc_.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        while (stack.size() > 1 && stack.back().first >= h->level) stack.pop_back();
        XmlNode outline = stack.back().second.append_child("outline");
        outline.set_attribute("text", inline_line(h->content));
        stack.emplace_back(h->level, outline);
        continue;
      }
      add_block(stack.back().second, b, 0);
    }
    util::XmlWriteOptions options;
    return util::xml_to_string(xml, options);
  }

 private:
  void item(XmlNode parent, const ir::Inlines& content, std::string_view note) {
    XmlNode outline = parent.append_child("outline");
    // A single link: a link outline.
    if (content.size() == 1) {
      if (const auto* link = content.front().get_if<ir::Link>(); link != nullptr && link->note.empty()) {
        outline.set_attribute("text", ir::plain_text(link->children));
        outline.set_attribute("type", "link");
        outline.set_attribute("url", link->url);
        return;
      }
    }
    outline.set_attribute("text", inline_line(content));
    if (!note.empty()) outline.set_attribute("_note", note);
  }

  void add_block(XmlNode parent, const ir::Block& b, int depth) {
    if (depth > max_depth) return;
    if (const auto* p = b.get_if<ir::Paragraph>()) {
      item(parent, p->content, {});
    } else if (const auto* list = b.get_if<ir::List>()) {
      for (const ir::Blocks& entry : list->items) {
        if (entry.empty()) continue;
        std::size_t first = 0;
        ir::Inlines text;
        if (const auto* para = entry.front().get_if<ir::Paragraph>()) {
          text = para->content;
          first = 1;
        }
        item(parent, text, {});
        XmlNode outline = parent.last_child();
        for (std::size_t k = first; k < entry.size(); ++k) add_block(outline, entry[k], depth + 1);
      }
    } else if (const auto* quote = b.get_if<ir::BlockQuote>()) {
      for (const ir::Block& inner : quote->blocks) add_block(parent, inner, depth + 1);
    } else if (const auto* code = b.get_if<ir::CodeBlock>()) {
      if (code->raw_format.empty()) item(parent, {ir::Code{code->code}}, {});
    } else if (const auto* table = b.get_if<ir::TableBlock>()) {
      for (const ir::TableRow& row : table->rows) {
        std::string line;
        for (const ir::TableCell& cell : row.cells) line += (line.empty() ? "" : " | ") + inline_line(cell.content);
        item(parent, {ir::Text{line}}, {});
      }
    } else if (const auto* image = b.get_if<ir::ImageBlock>()) {
      const std::string text = image->caption.empty() ? image->alt : image->caption;
      if (!text.empty()) item(parent, {ir::Text{text}}, {});
    }
  }

  const ir::Document& doc_;
};

// ============================================================================ ENEX

Result<ir::Document> read_enex(ByteView data, ReadContext& ctx) {
  CVT_TRY_ASSIGN(XmlDocument xml, parse(data));
  XmlNode root = xml.root();
  if (root.local_name() != "en-export") return fail(Status::Decode, "not an Evernote export");
  ir::Document document;
  std::size_t count = 0;
  for (XmlNode note : root.children_local("note")) {
    CVT_TRY(ctx.limits.check_archive_entries(++count));
    // Resources by MD5 of their data.
    std::map<std::string, ir::EncodedImage> resources;
    std::uint64_t total = 0;
    for (XmlNode resource : note.children_local("resource")) {
      std::string encoded;
      for (const char c : resource.child_local("data").text()) {
        if (!util::is_ascii_space(c)) encoded.push_back(c);
      }
      total += encoded.size();
      CVT_TRY(ctx.limits.check_decompressed(total));
      auto bytes = util::base64_decode(encoded);
      if (!bytes) continue;
      ir::EncodedImage image;
      image.mime = collapse_whitespace(resource.child_local("mime").text());
      const auto digest = util::md5(*bytes);
      const std::string key = util::hex_encode(as_bytes(std::span<const std::uint8_t>(digest)));
      image.bytes = std::move(*bytes);
      resources[key] = std::move(image);
    }
    const std::string title = collapse_whitespace(note.child_local("title").text());
    if (!title.empty()) document.blocks.push_back(ir::Heading{1, {ir::Text{title}}});
    if (document.metadata.title.empty() && root.children_local("note").begin() != root.children_local("note").end()) {
      document.metadata.title = title;
    }
    std::string content = note.child_local("content").text();
    // ENML: <en-media hash=..> becomes <img src="cid:hash">, <en-todo> a box.
    std::string html;
    std::size_t i = 0;
    while (i < content.size()) {
      const std::size_t at = content.find("<en-", i);
      if (at == std::string::npos) {
        html += content.substr(i);
        break;
      }
      html += content.substr(i, at - i);
      const std::size_t end = content.find('>', at);
      if (end == std::string::npos) break;
      const std::string tag = content.substr(at, end - at + 1);
      if (tag.starts_with("<en-media")) {
        const std::size_t h = tag.find("hash=\"");
        const std::string hash = h == std::string::npos ? std::string() : tag.substr(h + 6, tag.find('"', h + 6) - h - 6);
        html += "<img src=\"cid:" + hash + "\">";
      } else if (tag.starts_with("<en-todo")) {
        html += tag.find("checked=\"true\"") != std::string::npos ? "\xE2\x98\x92 " : "\xE2\x98\x90 ";
      } else if (tag.starts_with("<en-note")) {
        html += "<div>";
      }
      i = end + 1;
    }
    html = util::replace_all(html, "</en-note>", "</div>");
    HtmlReadOptions options;
    options.read_metadata = false;
    options.resolve = [&resources](std::string_view url) -> std::optional<ir::EncodedImage> {
      if (!url.starts_with("cid:")) return std::nullopt;
      const auto it = resources.find(std::string(url.substr(4)));
      if (it == resources.end()) return std::nullopt;
      return it->second;
    };
    if (auto parsed = parse_html_text(html, options)) {
      for (ir::Block& b : parsed->blocks) document.blocks.push_back(std::move(b));
    }
  }
  return document;
}

// ============================================================================ DocBook writer

class DocBookWriter {
 public:
  explicit DocBookWriter(const ir::Document& document) : doc_(document) {}

  std::string run() {
    XmlDocument xml;
    xml.set_declaration();
    XmlNode article = xml.append_child("article");
    article.set_attribute("xmlns", "http://docbook.org/ns/docbook");
    article.set_attribute("xmlns:xlink", "http://www.w3.org/1999/xlink");
    article.set_attribute("version", "5.0");
    if (!doc_.metadata.language.empty()) article.set_attribute("xml:lang", doc_.metadata.language);
    XmlNode info = article.append_child("info");
    info.append_child("title").set_text(document_title(doc_));
    if (!doc_.metadata.author.empty()) {
      info.append_child("author").append_child("personname").set_text(doc_.metadata.author);
    }
    if (!doc_.metadata.date.empty()) info.append_child("date").set_text(doc_.metadata.date);
    std::vector<std::pair<int, XmlNode>> stack{{0, article}};
    for (const ir::Block& b : doc_.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        while (stack.size() > 1 && stack.back().first >= h->level) stack.pop_back();
        XmlNode section = stack.back().second.append_child("section");
        if (!b.attr.id.empty() && util::xml_is_name(b.attr.id)) section.set_attribute("xml:id", b.attr.id);
        inlines(section.append_child("title"), h->content, 0);
        stack.emplace_back(h->level, section);
        continue;
      }
      block(stack.back().second, b, 0);
    }
    util::XmlWriteOptions options;
    return util::xml_to_string(xml, options);
  }

 private:
  void block(XmlNode parent, const ir::Block& b, int depth) {
    if (depth > max_depth) return;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            inlines(parent.append_child("para"), node.content, depth);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            inlines(parent.append_child("bridgehead"), node.content, depth);
          } else if constexpr (std::same_as<T, ir::List>) {
            if (node.definition) {
              XmlNode list = parent.append_child("variablelist");
              for (const ir::Blocks& item : node.items) {
                XmlNode entry = list.append_child("varlistentry");
                XmlNode term = entry.append_child("term");
                if (!item.empty()) {
                  if (const auto* p = item.front().get_if<ir::Paragraph>()) inlines(term, p->content, depth);
                }
                XmlNode def = entry.append_child("listitem");
                for (std::size_t k = 1; k < item.size(); ++k) block(def, item[k], depth + 1);
                if (item.size() <= 1) def.append_child("para");
              }
              return;
            }
            XmlNode list = parent.append_child(node.ordered ? "orderedlist" : "itemizedlist");
            if (node.ordered && node.start != 1) list.set_attribute("startingnumber", std::to_string(node.start));
            list.set_attribute("spacing", node.loose ? "normal" : "compact");
            for (const ir::Blocks& item : node.items) {
              XmlNode li = list.append_child("listitem");
              for (const ir::Block& inner : item) block(li, inner, depth + 1);
              if (item.empty()) li.append_child("para");
            }
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (node.math != ir::MathMode::None) {
              parent.append_child("informalequation").append_child("mathphrase").set_text(node.code);
              return;
            }
            if (!node.raw_format.empty()) return;
            XmlNode code = parent.append_child("programlisting");
            if (!node.language.empty()) code.set_attribute("language", node.language);
            code.append_text(node.code);
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            std::string tag = "blockquote";
            if (b.attr.has_class(ir::conventions::admonition)) {
              tag = "note";
              for (const std::string& c : b.attr.classes) {
                if (c == "warning" || c == "tip" || c == "important" || c == "caution" || c == "danger") tag = c;
              }
            }
            XmlNode quote = parent.append_child(tag);
            for (const ir::Block& inner : node.blocks) block(quote, inner, depth + 1);
            if (node.blocks.empty()) quote.append_child("para");
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            table(parent, node, depth);
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string url = image_url(node.source, true);
            XmlNode figure = node.caption.empty() ? parent.append_child("informalfigure") : parent.append_child("figure");
            if (!node.caption.empty()) figure.append_child("title").set_text(node.caption);
            XmlNode media = figure.append_child("mediaobject");
            if (!url.empty()) media.append_child("imageobject").append_child("imagedata").set_attribute("fileref", url);
            media.append_child("textobject").append_child("phrase").set_text(node.alt);
          } else if constexpr (std::same_as<T, ir::HorizontalRule>) {
            parent.append_pi("hr");
          } else if constexpr (std::same_as<T, ir::PageBreak>) {
            parent.append_pi("hard-pagebreak");
          }
        },
        b.node);
  }

  void table(XmlNode parent, const ir::TableBlock& t, int depth) {
    const TableGrid grid = table_grid(t);
    XmlNode table = t.caption.empty() ? parent.append_child("informaltable") : parent.append_child("table");
    if (!t.caption.empty()) inlines(table.append_child("title"), t.caption, depth);
    XmlNode group = table.append_child("tgroup");
    group.set_attribute("cols", std::to_string(std::max<std::size_t>(grid.columns, 1)));
    for (std::size_t c = 0; c < grid.columns; ++c) {
      XmlNode spec = group.append_child("colspec");
      spec.set_attribute("colname", "c" + std::to_string(c + 1));
      const ir::Alignment a = t.column_alignment(c);
      if (a != ir::Alignment::Default) spec.set_attribute("align", a == ir::Alignment::Left ? "left" : a == ir::Alignment::Right ? "right" : "center");
    }
    const std::size_t header = header_row_count(t);
    XmlNode head;
    XmlNode bodypart;
    for (std::size_t r = 0; r < t.rows.size(); ++r) {
      XmlNode part;
      if (r < header) {
        if (!head) head = group.append_child("thead");
        part = head;
      } else {
        if (!bodypart) bodypart = group.append_child("tbody");
        part = bodypart;
      }
      XmlNode row = part.append_child("row");
      std::size_t column = 0;
      for (const ir::TableCell& cell : t.rows[r].cells) {
        while (column < grid.columns && grid.cells[r][column] != &cell) ++column;
        XmlNode entry = row.append_child("entry");
        if (cell.colspan > 1) {
          entry.set_attribute("namest", "c" + std::to_string(column + 1));
          entry.set_attribute("nameend", "c" + std::to_string(std::min(column + static_cast<std::size_t>(cell.colspan), grid.columns)));
        }
        if (cell.rowspan > 1) entry.set_attribute("morerows", std::to_string(cell.rowspan - 1));
        inlines(entry, cell.content, depth);
      }
    }
    if (!bodypart) group.append_child("tbody").append_child("row").append_child("entry");
  }

  void inlines(XmlNode parent, const ir::Inlines& list, int depth) {
    if (depth > max_depth * 2) return;
    for (const ir::Inline& item : list) {
      std::visit(
          [&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            const auto wrap = [&](std::string_view tag, std::string_view role, const ir::Inlines& children) {
              XmlNode e = parent.append_child(tag);
              if (!role.empty()) e.set_attribute("role", role);
              inlines(e, children, depth + 1);
            };
            if constexpr (std::same_as<T, ir::Text>) {
              parent.append_text(node.text);
            } else if constexpr (std::same_as<T, ir::Emphasis>) {
              wrap("emphasis", {}, node.children);
            } else if constexpr (std::same_as<T, ir::Strong>) {
              wrap("emphasis", item.attr.has_class("mark") ? "highlight" : "strong", node.children);
            } else if constexpr (std::same_as<T, ir::Strikethrough>) {
              wrap("emphasis", "strikethrough", node.children);
            } else if constexpr (std::same_as<T, ir::Underline>) {
              wrap("emphasis", "underline", node.children);
            } else if constexpr (std::same_as<T, ir::Superscript>) {
              wrap("superscript", {}, node.children);
            } else if constexpr (std::same_as<T, ir::Subscript>) {
              wrap("subscript", {}, node.children);
            } else if constexpr (std::same_as<T, ir::Code>) {
              if (node.math != ir::MathMode::None) {
                parent.append_child("inlineequation").append_child("mathphrase").set_text(node.text);
              } else if (node.raw_format.empty()) {
                parent.append_child("literal").set_text(node.text);
              }
            } else if constexpr (std::same_as<T, ir::Link>) {
              if (!node.note.empty()) {
                if (const ir::Footnote* note = find_footnote(doc_, node.note); note != nullptr && depth < 4) {
                  XmlNode fn = parent.append_child("footnote");
                  for (const ir::Block& b : note->blocks) block(fn, b, depth + 1);
                  if (note->blocks.empty()) fn.append_child("para");
                  return;
                }
              }
              XmlNode link = parent.append_child("link");
              if (node.url.starts_with("#") && util::xml_is_name(node.url.substr(1))) {
                link.set_attribute("linkend", node.url.substr(1));
              } else {
                link.set_attribute("xlink:href", node.url);
              }
              inlines(link, node.children, depth + 1);
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              parent.append_text("\n");
            } else if constexpr (std::same_as<T, ir::InlineImage>) {
              const std::string url = image_url(node.source, true);
              XmlNode media = parent.append_child("inlinemediaobject");
              if (!url.empty()) media.append_child("imageobject").append_child("imagedata").set_attribute("fileref", url);
              media.append_child("textobject").append_child("phrase").set_text(node.alt);
            }
          },
          item.node);
    }
  }

  const ir::Document& doc_;
};

// ============================================================================ FB2 writer

class Fb2Writer {
 public:
  explicit Fb2Writer(const ir::Document& document) : doc_(document) {}

  std::string run() {
    XmlDocument xml;
    xml.set_declaration();
    XmlNode book = xml.append_child("FictionBook");
    book.set_attribute("xmlns", "http://www.gribuser.ru/xml/fictionbook/2.0");
    book.set_attribute("xmlns:l", "http://www.w3.org/1999/xlink");
    XmlNode description = book.append_child("description");
    XmlNode info = description.append_child("title-info");
    info.append_child("genre").set_text("unrecognised");
    XmlNode author = info.append_child("author");
    author.append_child("nickname").set_text(doc_.metadata.author.empty() ? std::string("Unknown") : doc_.metadata.author);
    const std::string title = document_title(doc_);
    info.append_child("book-title").set_text(title.empty() ? std::string("Untitled") : title);
    if (const auto it = doc_.metadata.extra.find("description"); it != doc_.metadata.extra.end()) {
      info.append_child("annotation").append_child("p").set_text(it->second);
    }
    if (const auto it = doc_.metadata.extra.find("keywords"); it != doc_.metadata.extra.end()) info.append_child("keywords").set_text(it->second);
    if (!doc_.metadata.date.empty()) info.append_child("date").set_text(doc_.metadata.date);
    info.append_child("lang").set_text(doc_.metadata.language.empty() ? std::string("und") : doc_.metadata.language.substr(0, doc_.metadata.language.find('-')));
    XmlNode body = book.append_child("body");
    std::vector<std::pair<int, XmlNode>> stack{{0, body}};
    XmlNode current = body.append_child("section");
    stack.emplace_back(0, current);
    bool section_has_content = false;
    for (const ir::Block& b : doc_.blocks) {
      if (const auto* h = b.get_if<ir::Heading>()) {
        while (stack.size() > 1 && (stack.back().first >= h->level || (stack.back().first == 0 && !section_has_content && stack.size() > 2))) {
          stack.pop_back();
        }
        if (stack.size() == 2 && stack.back().first == 0 && !section_has_content) stack.pop_back();
        XmlNode section = stack.back().second.append_child("section");
        if (!b.attr.id.empty() && util::xml_is_name(b.attr.id)) section.set_attribute("id", b.attr.id);
        inlines(section.append_child("title").append_child("p"), h->content, 0);
        stack.emplace_back(h->level, section);
        section_has_content = true;
        continue;
      }
      block(stack.back().second, b, 0);
      section_has_content = true;
    }
    // Every section needs content.
    for (XmlNode section : body.descendants()) {
      if (section.is_element() && section.local_name() == "section" && !section.first_child()) section.append_child("empty-line");
    }
    if (!doc_.footnotes.empty()) {
      XmlNode notes = book.append_child("body");
      notes.set_attribute("name", "notes");
      for (std::size_t i = 0; i < doc_.footnotes.size(); ++i) {
        XmlNode section = notes.append_child("section");
        section.set_attribute("id", "n" + std::to_string(i + 1));
        section.append_child("title").append_child("p").set_text(std::to_string(i + 1));
        for (const ir::Block& b : doc_.footnotes[i].blocks) block(section, b, 1);
        if (doc_.footnotes[i].blocks.empty()) section.append_child("empty-line");
      }
    }
    for (std::size_t i = 0; i < binaries_.size(); ++i) {
      XmlNode binary = book.append_child("binary");
      binary.set_attribute("id", "img" + std::to_string(i + 1));
      binary.set_attribute("content-type", binaries_[i].mime);
      binary.set_text(util::base64_encode(binaries_[i].bytes));
    }
    util::XmlWriteOptions options;
    return util::xml_to_string(xml, options);
  }

 private:
  std::string image_href(const ir::ImageSource& source) {
    if (const auto* e = std::get_if<ir::EncodedImage>(&source); e != nullptr && !e->url.empty() && e->bytes.empty()) return e->url;
    auto encoded = encoded_image(source);
    if (!encoded) return {};
    binaries_.push_back(std::move(*encoded));
    return "#img" + std::to_string(binaries_.size());
  }

  void paragraph(XmlNode parent, const ir::Inlines& content, int depth) { inlines(parent.append_child("p"), content, depth); }

  void block(XmlNode parent, const ir::Block& b, int depth) {
    if (depth > max_depth) return;
    std::visit(
        [&](const auto& node) {
          using T = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::same_as<T, ir::Paragraph>) {
            paragraph(parent, node.content, depth);
          } else if constexpr (std::same_as<T, ir::Heading>) {
            inlines(parent.append_child("subtitle"), node.content, depth);
          } else if constexpr (std::same_as<T, ir::List>) {
            int number = node.start;
            for (std::size_t i = 0; i < node.items.size(); ++i) {
              const ir::Blocks& item = node.items[i];
              std::string marker = node.definition ? std::string() : node.ordered ? std::to_string(number++) + ". " : "\xE2\x80\xA2 ";
              bool first = true;
              for (const ir::Block& inner : item) {
                if (first && inner.is<ir::Paragraph>()) {
                  ir::Inlines content;
                  if (!marker.empty()) content.push_back(ir::Text{marker});
                  const ir::Inlines& p = inner.get_if<ir::Paragraph>()->content;
                  if (node.definition) content.push_back(ir::Strong{p});
                  else content.insert(content.end(), p.begin(), p.end());
                  paragraph(parent, content, depth);
                } else {
                  XmlNode cite = parent.append_child("cite");
                  block(cite, inner, depth + 1);
                }
                first = false;
              }
            }
          } else if constexpr (std::same_as<T, ir::CodeBlock>) {
            if (!node.raw_format.empty()) return;
            for (const std::string_view line : util::split(node.code, '\n')) {
              XmlNode p = parent.append_child("p");
              if (!line.empty()) p.append_child("code").set_text(line);
            }
          } else if constexpr (std::same_as<T, ir::BlockQuote>) {
            XmlNode cite = parent.append_child("cite");
            for (const ir::Block& inner : node.blocks) block(cite, inner, depth + 1);
            if (node.blocks.empty()) cite.append_child("empty-line");
          } else if constexpr (std::same_as<T, ir::TableBlock>) {
            XmlNode table = parent.append_child("table");
            for (const ir::TableRow& row : node.rows) {
              XmlNode tr = table.append_child("tr");
              for (const ir::TableCell& cell : row.cells) {
                XmlNode c = tr.append_child(row.header ? "th" : "td");
                if (cell.colspan > 1) c.set_attribute("colspan", std::to_string(cell.colspan));
                if (cell.rowspan > 1) c.set_attribute("rowspan", std::to_string(cell.rowspan));
                inlines(c, cell.content, depth);
              }
            }
          } else if constexpr (std::same_as<T, ir::ImageBlock>) {
            const std::string href = image_href(node.source);
            if (href.empty()) return;
            XmlNode image = parent.append_child("image");
            image.set_attribute("l:href", href);
            if (!node.alt.empty()) image.set_attribute("alt", node.alt);
            if (!node.caption.empty()) image.set_attribute("title", node.caption);
          } else if constexpr (std::same_as<T, ir::HorizontalRule> || std::same_as<T, ir::PageBreak>) {
            parent.append_child("empty-line");
          }
        },
        b.node);
  }

  void inlines(XmlNode parent, const ir::Inlines& list, int depth) {
    if (depth > max_depth * 2) return;
    for (const ir::Inline& item : list) {
      std::visit(
          [&](const auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            const auto wrap = [&](std::string_view tag, const ir::Inlines& children) { inlines(parent.append_child(tag), children, depth + 1); };
            if constexpr (std::same_as<T, ir::Text>) {
              parent.append_text(node.text);
            } else if constexpr (std::same_as<T, ir::Emphasis> || std::same_as<T, ir::Underline>) {
              wrap("emphasis", node.children);
            } else if constexpr (std::same_as<T, ir::Strong>) {
              wrap("strong", node.children);
            } else if constexpr (std::same_as<T, ir::Strikethrough>) {
              wrap("strikethrough", node.children);
            } else if constexpr (std::same_as<T, ir::Superscript>) {
              wrap("sup", node.children);
            } else if constexpr (std::same_as<T, ir::Subscript>) {
              wrap("sub", node.children);
            } else if constexpr (std::same_as<T, ir::Code>) {
              if (node.raw_format.empty()) parent.append_child("code").set_text(node.text);
            } else if constexpr (std::same_as<T, ir::Link>) {
              if (!node.note.empty()) {
                const std::size_t index = footnote_index(doc_, node.note);
                if (index > 0) {
                  XmlNode a = parent.append_child("a");
                  a.set_attribute("l:href", "#n" + std::to_string(index));
                  a.set_attribute("type", "note");
                  a.set_text("[" + std::to_string(index) + "]");
                  return;
                }
              }
              XmlNode a = parent.append_child("a");
              a.set_attribute("l:href", node.url);
              inlines(a, node.children, depth + 1);
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              parent.append_text(" ");
            } else if constexpr (std::same_as<T, ir::InlineImage>) {
              const std::string href = image_href(node.source);
              if (href.empty()) {
                parent.append_text(node.alt);
                return;
              }
              XmlNode image = parent.append_child("image");
              image.set_attribute("l:href", href);
              if (!node.alt.empty()) image.set_attribute("alt", node.alt);
            }
          },
          item.node);
    }
  }

  const ir::Document& doc_;
  std::vector<ir::EncodedImage> binaries_;
};

// ============================================================================ registration

std::string root_name(ByteView data) {
  const std::string_view head = sniff_head(data, 4096);
  std::size_t i = 0;
  while (i < head.size()) {
    const std::size_t lt = head.find('<', i);
    if (lt == std::string_view::npos || lt + 1 >= head.size()) return {};
    const char c = head[lt + 1];
    if (c == '?' || c == '!') {
      if (head.substr(lt).starts_with("<!--")) {
        const std::size_t end = head.find("-->", lt);
        if (end == std::string_view::npos) return {};
        i = end + 3;
      } else {
        // Doctype may contain an internal subset.
        const std::size_t end = head.find('>', lt);
        if (end == std::string_view::npos) return {};
        i = end + 1;
      }
      continue;
    }
    std::size_t end = lt + 1;
    while (end < head.size() && !util::is_ascii_space(head[end]) && head[end] != '>' && head[end] != '/') ++end;
    std::string name(head.substr(lt + 1, end - lt - 1));
    if (const std::size_t colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
    return name;
  }
  return {};
}

int sniff_root(ByteView data, std::string_view filename, std::initializer_list<std::string_view> roots,
               std::initializer_list<std::string_view> extensions, std::string_view marker = {}) {
  const std::string root = root_name(data);
  const bool root_ok = std::ranges::find(roots, root) != roots.end();
  const bool marker_ok = marker.empty() || sniff_head(data, 4096).find(marker) != std::string_view::npos;
  if (root_ok && marker_ok) return has_extension(filename, extensions) ? 95 : 90;
  if (has_extension(filename, extensions) && root_ok) return 80;
  return 0;
}

template <class Reader>
MarkupFormat xml_format(std::string id, std::string name, std::string description, std::vector<std::string> extensions,
                        std::string mime, int popularity) {
  MarkupFormat format;
  format.id = std::move(id);
  format.name = std::move(name);
  format.description = std::move(description);
  format.extensions = std::move(extensions);
  format.mime = std::move(mime);
  format.popularity = popularity;
  format.read_bytes = [](ByteView data, ReadContext&) -> Result<ir::Document> {
    CVT_TRY_ASSIGN(XmlDocument xml, parse(data));
    ir::Document document;
    Reader reader(document);
    reader.read(xml.root());
    return document;
  };
  return format;
}

}  // namespace

void register_xml_formats(Registry& registry) {
  MarkupFormat docbook = xml_format<DocBookReader>("docbook", "DocBook", "DocBook 5 (and 4) XML documentation.",
                                                   {"dbk", "docbook", "xml"}, "application/docbook+xml", 25);
  docbook.sniff = [](ByteView data, std::string_view filename) {
    const int by_root = sniff_root(data, filename, {"article", "book", "chapter", "section", "refentry"}, {"dbk", "docbook"},
                                   "docbook");
    if (by_root > 0) return by_root;
    // DocBook 4 without the namespace.
    const std::string_view head = sniff_head(data, 2048);
    if (head.find("DocBook") != std::string_view::npos && head.find("<!DOCTYPE") != std::string_view::npos) return 88;
    return has_extension(filename, {"dbk", "docbook"}) ? 80 : 0;
  };
  docbook.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    DocBookWriter writer(document);
    return writer.run();
  };
  docbook.binary_output = true;
  register_markup(registry, std::move(docbook));

  MarkupFormat jats = xml_format<JatsReader>("jats", "JATS article", "Journal Article Tag Suite XML (read).",
                                             {"jats", "nxml", "xml"}, "application/jats+xml", 15);
  jats.sniff = [](ByteView data, std::string_view filename) {
    const std::string_view head = sniff_head(data, 4096);
    if (root_name(data) == "article" && (head.find("JATS") != std::string_view::npos || head.find("<front>") != std::string_view::npos ||
                                         head.find("article-meta") != std::string_view::npos || head.find("dtd-version") != std::string_view::npos)) {
      return 91;
    }
    return has_extension(filename, {"jats", "nxml"}) ? 80 : 0;
  };
  register_markup(registry, std::move(jats));

  MarkupFormat tei = xml_format<TeiReader>("tei", "TEI document", "Text Encoding Initiative XML (read).", {"tei", "xml"},
                                           "application/tei+xml", 12);
  tei.sniff = [](ByteView data, std::string_view filename) {
    return sniff_root(data, filename, {"TEI", "TEI.2", "teiCorpus"}, {"tei"});
  };
  register_markup(registry, std::move(tei));

  MarkupFormat fb2;
  fb2.id = "fb2";
  fb2.name = "FictionBook e-book";
  fb2.description = "FictionBook 2 XML e-book with embedded pictures.";
  fb2.category = Category::Ebook;
  fb2.extensions = {"fb2"};
  fb2.mime = "application/x-fictionbook+xml";
  fb2.popularity = 25;
  fb2.sniff = [](ByteView data, std::string_view filename) { return sniff_root(data, filename, {"FictionBook"}, {"fb2"}); };
  fb2.read_bytes = [](ByteView data, ReadContext& ctx) -> Result<ir::Document> {
    CVT_TRY_ASSIGN(XmlDocument xml, parse(data));
    if (xml.root().local_name() != "FictionBook") return fail(Status::Decode, "not a FictionBook document");
    ir::Document document;
    Fb2Reader reader(document);
    CVT_TRY(reader.read(xml.root(), ctx));
    return document;
  };
  fb2.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    Fb2Writer writer(document);
    return writer.run();
  };
  fb2.binary_output = true;
  fb2.write_lossy = true;  // lists become marked paragraphs
  register_markup(registry, std::move(fb2));

  MarkupFormat opml;
  opml.id = "opml";
  opml.name = "OPML outline";
  opml.description = "Outline Processor Markup Language: outlines as nested lists.";
  opml.extensions = {"opml"};
  opml.mime = "text/x-opml";
  opml.popularity = 15;
  opml.sniff = [](ByteView data, std::string_view filename) { return sniff_root(data, filename, {"opml"}, {"opml"}); };
  opml.read_bytes = read_opml;
  opml.write = [](const ir::Document& document, WriteContext&) -> Result<std::string> {
    OpmlWriter writer(document);
    return writer.run();
  };
  opml.write_lossy = true;
  opml.binary_output = true;
  register_markup(registry, std::move(opml));

  MarkupFormat enex;
  enex.id = "enex";
  enex.name = "Evernote export";
  enex.description = "Notes exported from Evernote (read).";
  enex.extensions = {"enex"};
  enex.mime = "application/enex+xml";
  enex.popularity = 12;
  enex.sniff = [](ByteView data, std::string_view filename) { return sniff_root(data, filename, {"en-export"}, {"enex"}); };
  enex.read_bytes = read_enex;
  register_markup(registry, std::move(enex));
}

}  // namespace cvt::document
