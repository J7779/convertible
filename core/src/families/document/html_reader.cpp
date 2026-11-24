// Convertible core - document family: HTML DOM -> ir::Document.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <set>

#include "families/document/doc_common.hpp"
#include "families/document/document_api.hpp"
#include "families/document/html_internal.hpp"
#include "util/text.hpp"

namespace cvt::document {

namespace {

using html::Dom;
using html::Node;
using html::NodeId;
using html::NodeType;

bool is_block_element(std::string_view n) {
  static const std::set<std::string_view, std::less<>> names = {
      "address", "article", "aside",  "blockquote", "body",    "center",  "details", "dialog", "dir",   "div",
      "dl",      "dd",      "dt",     "fieldset",   "figcaption", "figure", "footer", "form",  "h1",    "h2",
      "h3",      "h4",      "h5",     "h6",         "header",  "hgroup",  "hr",      "li",     "main",  "menu",
      "nav",     "ol",      "p",      "pre",        "section", "summary", "table",   "ul",     "listing", "xmp",
      "plaintext", "legend", "caption", "tbody",    "thead",   "tfoot",   "tr",      "td",     "th",    "html",
      "noscript", "search", "article", "frameset",  "map",     "optgroup", "select", "video",  "audio", "object"};
  return names.contains(n);
}

std::string lower_copy(std::string_view text) { return util::to_lower(text); }

bool has_class(const Dom& dom, NodeId id, std::string_view name) {
  for (const std::string_view c : util::split(dom.attribute(id, "class"), ' ', false)) {
    if (c == name) return true;
  }
  return false;
}

std::string declaration_value(std::string_view declarations, std::string_view property) {
  for (const std::string_view declaration : util::split(declarations, ';', false)) {
    const std::size_t colon = declaration.find(':');
    if (colon == std::string_view::npos) continue;
    if (util::trim(declaration.substr(0, colon)) == property) return std::string(util::trim(declaration.substr(colon + 1)));
  }
  return {};
}

// Inline style first, then the class rules of the page's stylesheet.
std::string style_value(const Dom& dom, NodeId id, std::string_view property) {
  std::string value = declaration_value(lower_copy(dom.attribute(id, "style")), property);
  if (!value.empty() || dom.class_rules.empty()) return value;
  for (const std::string_view c : util::split(dom.attribute(id, "class"), ' ', false)) {
    const auto it = dom.class_rules.find(c);
    if (it != dom.class_rules.end()) {
      value = declaration_value(it->second, property);
      if (!value.empty()) return value;
    }
  }
  return value;
}

ir::Alignment alignment_of(const Dom& dom, NodeId id) {
  std::string value = lower_copy(dom.attribute(id, "align"));
  if (value.empty()) value = style_value(dom, id, "text-align");
  if (value == "left" || value == "start") return ir::Alignment::Left;
  if (value == "center") return ir::Alignment::Center;
  if (value == "right" || value == "end") return ir::Alignment::Right;
  return ir::Alignment::Default;
}

int int_attribute(const Dom& dom, NodeId id, std::string_view name, int fallback, int low, int high) {
  const auto parsed = util::parse_int(util::trim(dom.attribute(id, name)));
  if (!parsed) return fallback;
  return static_cast<int>(std::clamp<std::int64_t>(*parsed, low, high));
}

// Strip "\(" "\)" / "\[" "\]" / "$" delimiters of rendered math.
std::string strip_math_delimiters(std::string text) {
  std::string_view t = util::trim(text);
  if ((t.starts_with("\\(") && t.ends_with("\\)")) || (t.starts_with("\\[") && t.ends_with("\\]"))) {
    t = t.substr(2, t.size() - 4);
  } else if (t.starts_with("$$") && t.ends_with("$$") && t.size() >= 4) {
    t = t.substr(2, t.size() - 4);
  } else if (t.starts_with("$") && t.ends_with("$") && t.size() >= 2) {
    t = t.substr(1, t.size() - 2);
  }
  return std::string(util::trim(t));
}

class Converter {
 public:
  Converter(const Dom& dom, const HtmlReadOptions& options, WarningSink warnings)
      : dom_(dom), options_(options), warnings_(warnings) {}

  ir::Document run() {
    read_metadata();
    // Footnote sections first, so references can be resolved.
    collect_notes(0);
    ir::Blocks blocks;
    convert_children(0, blocks, 0);
    document_.blocks = std::move(blocks);
    resolve_notes();
    if (dropped_media_ > 0 && warnings_ != nullptr) {
      warnings_->push_back(std::to_string(dropped_media_) + " embedded media element(s) (video, audio, frames) were left out.");
    }
    return std::move(document_);
  }

 private:
  // ---- metadata -------------------------------------------------------------

  void read_metadata() {
    if (!options_.read_metadata) return;
    const std::string_view lang = dom_.attribute(0, "lang");
    if (!lang.empty()) document_.metadata.language = std::string(util::trim(lang));
    const NodeId title = dom_.find_first("title");
    if (title != html::no_node) document_.metadata.title = collapse_whitespace(dom_.text_content(title));
    std::vector<NodeId> stack{0};
    while (!stack.empty()) {
      const NodeId id = stack.back();
      stack.pop_back();
      const Node& n = dom_.node(id);
      if (n.type == NodeType::Element && n.name == "meta") {
        const std::string name = lower_copy(dom_.attribute(id, dom_.has_attribute(id, "name") ? "name" : "property"));
        const std::string content(util::trim(dom_.attribute(id, "content")));
        if (content.empty()) continue;
        if (name == "author" || name == "dc.creator" || name == "dcterms.creator") {
          document_.metadata.author = content;
        } else if (name == "date" || name == "dc.date" || name == "dcterms.date" || name == "dcterms.created") {
          document_.metadata.date = content;
        } else if (name == "description" || name == "keywords" || name == "subject" || name == "dc.subject" ||
                   name == "dc.description" || name == "dcterms.description") {
          const std::string key = name.substr(name.find('.') == std::string::npos ? 0 : name.find('.') + 1);
          document_.metadata.extra[key] = content;
        } else if (name == "dc.language" || name == "dcterms.language") {
          if (document_.metadata.language.empty()) document_.metadata.language = content;
        }
        continue;
      }
      if (n.type == NodeType::Element && n.name == "body") break;
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
    }
  }

  // ---- footnotes --------------------------------------------------------------

  bool is_notes_section(NodeId id) const {
    const Node& n = dom_.node(id);
    if (n.type != NodeType::Element) return false;
    if (n.name != "section" && n.name != "div" && n.name != "aside" && n.name != "footer" && n.name != "ol") return false;
    const std::string_view role = dom_.attribute(id, "role");
    return role == "doc-endnotes" || role == "doc-footnotes" || has_class(dom_, id, "footnotes") ||
           has_class(dom_, id, "footnote-list") || dom_.attribute(id, "id") == "footnotes";
  }

  void collect_notes(NodeId root) {
    std::vector<NodeId> stack{root};
    while (!stack.empty()) {
      const NodeId id = stack.back();
      stack.pop_back();
      if (is_notes_section(id)) {
        notes_sections_.insert(id);
        // Every <li id=...> (or element with id) inside is a note.
        std::vector<NodeId> inner{id};
        while (!inner.empty()) {
          const NodeId k = inner.back();
          inner.pop_back();
          const Node& n = dom_.node(k);
          if (n.type == NodeType::Element && (n.name == "li" || n.name == "aside" || n.name == "div") &&
              !dom_.attribute(k, "id").empty() && k != id) {
            note_elements_.emplace_back(std::string(dom_.attribute(k, "id")), k);
            continue;
          }
          for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) inner.push_back(*it);
        }
        continue;
      }
      const Node& n = dom_.node(id);
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
    }
    for (std::size_t i = 0; i < note_elements_.size(); ++i) note_ids_[note_elements_[i].first] = i;
  }

  bool is_back_link(NodeId id) const {
    const Node& n = dom_.node(id);
    if (n.name != "a") return false;
    const std::string_view role = dom_.attribute(id, "role");
    if (role == "doc-backlink" || has_class(dom_, id, "footnote-back") || has_class(dom_, id, "footnote-backref") ||
        has_class(dom_, id, "reversefootnote")) {
      return true;
    }
    const std::string text = collapse_whitespace(dom_.text_content(id));
    return text == "\xE2\x86\xA9" || text == "\xE2\x86\xA9\xEF\xB8\x8E" || text == "\xE2\x86\xA9\xEF\xB8\x8F";
  }

  // An <a> that points at a known note.
  std::optional<std::string> note_target(NodeId id) const {
    const std::string_view href = dom_.attribute(id, "href");
    if (!href.starts_with("#")) return std::nullopt;
    const std::string target(href.substr(1));
    if (!note_ids_.contains(target)) return std::nullopt;
    const std::string_view role = dom_.attribute(id, "role");
    const bool marked = role == "doc-noteref" || has_class(dom_, id, "footnote-ref") ||
                        has_class(dom_, id, "footnote-reference") || has_class(dom_, id, "footnote");
    bool in_sup = false;
    const Node& n = dom_.node(id);
    if (n.parent != html::no_node && dom_.node(n.parent).name == "sup") in_sup = true;
    for (const NodeId child : n.children) {
      if (dom_.node(child).type == NodeType::Element && dom_.node(child).name == "sup") in_sup = true;
    }
    if (!marked && !in_sup) return std::nullopt;
    return target;
  }

  void resolve_notes() {
    // Bodies in order of the notes section; labels are renumbered later.
    for (const auto& [id, element] : note_elements_) {
      if (!referenced_.contains(id)) continue;
      ir::Footnote note;
      note.label = id;
      in_note_ = true;
      convert_children(element, note.blocks, 1);
      in_note_ = false;
      document_.footnotes.push_back(std::move(note));
    }
    if (!document_.footnotes.empty()) number_footnotes(document_);
  }

  // ---- blocks -------------------------------------------------------------------

  struct InlineContext {
    ir::Inlines* out;
    bool preformatted = false;
  };

  void flush(ir::Inlines& pending, ir::Blocks& out) {
    finish_inlines(pending);
    if (pending.empty()) return;
    // A paragraph that is just one picture is a picture block.
    if (pending.size() == 1) {
      if (auto* image = pending.front().get_if<ir::InlineImage>()) {
        ir::ImageBlock block{std::move(image->source), std::move(image->alt), {}, std::move(image->title)};
        out.push_back(std::move(block));
        pending.clear();
        return;
      }
    }
    out.push_back(ir::Paragraph{std::move(pending)});
    pending.clear();
  }

  void convert_children(NodeId parent, ir::Blocks& out, int depth) {
    ir::Inlines pending;
    for (const NodeId child : dom_.node(parent).children) convert_node(child, out, pending, depth);
    flush(pending, out);
  }

  bool contains_block(NodeId id, int budget = 400) const {
    std::vector<NodeId> stack(dom_.node(id).children.rbegin(), dom_.node(id).children.rend());
    while (!stack.empty() && budget-- > 0) {
      const NodeId k = stack.back();
      stack.pop_back();
      const Node& n = dom_.node(k);
      if (n.type == NodeType::Element) {
        if (is_block_element(n.name)) return true;
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
      }
    }
    return false;
  }

  void convert_node(NodeId id, ir::Blocks& out, ir::Inlines& pending, int depth) {
    const Node& n = dom_.node(id);
    if (n.type == NodeType::Text) {
      append_text(pending, n.text);
      return;
    }
    if (n.type != NodeType::Element) return;
    if (notes_sections_.contains(id)) return;
    const std::string& name = n.name;
    if (name == "head" || name == "title" || name == "meta" || name == "link" || name == "base") return;
    if (depth > ir::max_document_depth - 4) {
      // Too deep: keep the text.
      append_text(pending, dom_.text_content(id));
      return;
    }
    if (!is_block_element(name)) {
      if (contains_block(id) && name != "svg" && name != "math") {
        // An inline element wrapping blocks (<a><div>..</div></a>): transparent.
        for (const NodeId child : n.children) convert_node(child, out, pending, depth + 1);
        return;
      }
      convert_inline(id, pending, depth);
      return;
    }
    flush(pending, out);
    ir::Attributes attr;
    attr.id = std::string(dom_.attribute(id, "id"));
    if (is_heading_name(name)) {
      ir::Inlines content;
      convert_inline_children(id, content, depth);
      finish_inlines(content);
      out.push_back(ir::Block(ir::Heading{name[1] - '0', std::move(content)}, std::move(attr)));
      return;
    }
    if (name == "p") {
      ir::Inlines content;
      convert_inline_children(id, content, depth);
      if (has_class(dom_, id, "line-block")) attr.add_class(ir::conventions::line_block);
      finish_inlines(content);
      if (content.size() == 1 && content.front().is<ir::InlineImage>()) {
        flush(content, out);
        return;
      }
      if (!content.empty()) out.push_back(ir::Block(ir::Paragraph{std::move(content)}, std::move(attr)));
      return;
    }
    if (name == "ul" || name == "ol" || name == "menu" || name == "dir") {
      convert_list(id, out, depth, std::move(attr));
      return;
    }
    if (name == "dl") {
      convert_definition_list(id, out, depth);
      return;
    }
    if (name == "blockquote") {
      ir::BlockQuote quote;
      convert_children(id, quote.blocks, depth + 1);
      out.push_back(ir::Block(std::move(quote), std::move(attr)));
      return;
    }
    if (name == "pre" || name == "listing" || name == "xmp" || name == "plaintext") {
      convert_pre(id, out);
      return;
    }
    if (name == "hr") {
      out.push_back(ir::HorizontalRule{});
      return;
    }
    if (name == "table") {
      convert_table(id, out, depth);
      return;
    }
    if (name == "figure") {
      convert_figure(id, out, depth);
      return;
    }
    if (name == "video" || name == "audio" || name == "object" || name == "frameset") {
      ++dropped_media_;
      return;
    }
    if (name == "select" || name == "optgroup") return;
    if ((name == "div" || name == "aside" || name == "section") && is_admonition(id)) {
      ir::BlockQuote quote;
      convert_children(id, quote.blocks, depth + 1);
      ir::Attributes quote_attr;
      quote_attr.add_class(ir::conventions::admonition);
      for (const std::string_view c : util::split(dom_.attribute(id, "class"), ' ', false)) {
        if (c != "admonition") quote_attr.add_class(c);
      }
      out.push_back(ir::Block(std::move(quote), std::move(quote_attr)));
      return;
    }
    if ((name == "div" || name == "p") && (has_class(dom_, id, "math") && has_class(dom_, id, "display"))) {
      ir::CodeBlock math;
      math.math = ir::MathMode::Display;
      math.code = strip_math_delimiters(dom_.text_content(id));
      out.push_back(std::move(math));
      return;
    }
    const std::string page_break = style_value(dom_, id, "page-break-before") + style_value(dom_, id, "page-break-after") +
                                   style_value(dom_, id, "break-before") + style_value(dom_, id, "break-after");
    if (page_break.find("always") != std::string::npos || page_break.find("page") != std::string::npos ||
        has_class(dom_, id, "page-break") || has_class(dom_, id, "pagebreak")) {
      if (name == "div" && dom_.text_content(id).find_first_not_of(" \t\r\n") == std::string::npos) {
        out.push_back(ir::PageBreak{});
        return;
      }
    }
    if (name == "summary" || name == "legend" || name == "caption" || name == "figcaption") {
      ir::Inlines content;
      convert_inline_children(id, content, depth);
      finish_inlines(content);
      if (!content.empty()) out.push_back(ir::Paragraph{{ir::Strong{std::move(content)}}});
      return;
    }
    // Generic containers (div, section, article, li outside lists, td outside tables, ...).
    ir::Blocks inner;
    convert_children(id, inner, depth + 1);
    if (!attr.id.empty() && !inner.empty() && inner.front().attr.id.empty() && !name.starts_with("t")) {
      inner.front().attr.id = attr.id;
    }
    for (ir::Block& b : inner) out.push_back(std::move(b));
  }

  static bool is_heading_name(std::string_view name) {
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
  }

  bool is_admonition(NodeId id) const {
    static constexpr std::array<std::string_view, 9> kinds = {"admonition", "note",   "warning", "tip",   "important",
                                                              "caution",    "danger", "hint",    "callout"};
    for (const std::string_view c : util::split(dom_.attribute(id, "class"), ' ', false)) {
      if (std::ranges::find(kinds, c) != kinds.end()) return true;
    }
    return false;
  }

  void convert_list(NodeId id, ir::Blocks& out, int depth, ir::Attributes attr) {
    const Node& n = dom_.node(id);
    ir::List list;
    list.ordered = n.name == "ol";
    list.start = int_attribute(dom_, id, "start", 1, -1000000000, 1000000000);
    const std::string_view type = dom_.attribute(id, "type");
    if (type == "a") list.numbering = ir::ListNumbering::LowerAlpha;
    if (type == "A") list.numbering = ir::ListNumbering::UpperAlpha;
    if (type == "i") list.numbering = ir::ListNumbering::LowerRoman;
    if (type == "I") list.numbering = ir::ListNumbering::UpperRoman;
    bool any_task = false;
    std::vector<ir::TaskState> tasks;
    for (const NodeId child : n.children) {
      const Node& c = dom_.node(child);
      if (c.type == NodeType::Element && c.name == "li") {
        ir::Blocks item;
        const ir::TaskState state = task_state(child);
        if (state != ir::TaskState::None) any_task = true;
        convert_children(child, item, depth + 1);
        list.items.push_back(std::move(item));
        tasks.push_back(state);
      } else if (c.type == NodeType::Text && util::trim(c.text).empty()) {
        continue;
      } else {
        // Stray content inside the list: attach it to the last item.
        if (list.items.empty()) {
          list.items.emplace_back();
          tasks.push_back(ir::TaskState::None);
        }
        ir::Inlines pending;
        convert_node(child, list.items.back(), pending, depth + 1);
        flush(pending, list.items.back());
      }
    }
    if (any_task) list.tasks = std::move(tasks);
    if (list.items.empty()) return;
    out.push_back(ir::Block(std::move(list), std::move(attr)));
  }

  // A leading <input type="checkbox"> (GitHub, pandoc) makes a task item.
  ir::TaskState task_state(NodeId li) {
    std::vector<NodeId> stack{li};
    int budget = 6;
    while (!stack.empty() && budget-- > 0) {
      const NodeId id = stack.back();
      stack.pop_back();
      const Node& n = dom_.node(id);
      if (n.type == NodeType::Text && !util::trim(n.text).empty()) return ir::TaskState::None;
      if (n.type == NodeType::Element && n.name == "input") {
        if (lower_copy(dom_.attribute(id, "type")) != "checkbox") return ir::TaskState::None;
        skipped_inputs_.insert(id);
        return dom_.has_attribute(id, "checked") ? ir::TaskState::Done : ir::TaskState::Open;
      }
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
    }
    return ir::TaskState::None;
  }

  void convert_definition_list(NodeId id, ir::Blocks& out, int depth) {
    ir::List list;
    list.definition = true;
    std::vector<NodeId> children = dom_.node(id).children;
    // <div> wrappers around dt/dd groups are allowed.
    std::vector<NodeId> flat;
    for (const NodeId child : children) {
      const Node& c = dom_.node(child);
      if (c.type == NodeType::Element && c.name == "div") {
        for (const NodeId g : c.children) flat.push_back(g);
      } else {
        flat.push_back(child);
      }
    }
    for (const NodeId child : flat) {
      const Node& c = dom_.node(child);
      if (c.type != NodeType::Element) continue;
      if (c.name == "dt") {
        ir::Inlines term;
        convert_inline_children(child, term, depth);
        finish_inlines(term);
        if (!list.items.empty() && list.items.back().size() == 1) {
          // Two terms in a row share a definition: join them.
          if (auto* p = list.items.back().front().get_if<ir::Paragraph>()) {
            p->content.push_back(ir::LineBreak{});
            for (ir::Inline& i : term) p->content.push_back(std::move(i));
            continue;
          }
        }
        list.items.push_back({ir::Paragraph{std::move(term)}});
      } else if (c.name == "dd") {
        if (list.items.empty()) list.items.push_back({ir::Paragraph{}});
        convert_children(child, list.items.back(), depth + 1);
      }
    }
    if (!list.items.empty()) out.push_back(std::move(list));
  }

  void convert_pre(NodeId id, ir::Blocks& out) {
    ir::CodeBlock block;
    std::string classes(dom_.attribute(id, "class"));
    // <pre><code class="language-x">: the code element carries the class.
    const Node& n = dom_.node(id);
    for (const NodeId child : n.children) {
      const Node& c = dom_.node(child);
      if (c.type == NodeType::Element && c.name == "code") {
        classes += " ";
        classes += dom_.attribute(child, "class");
      }
    }
    std::string text = dom_.text_content(id);
    // A newline right after <pre> is not content.
    if (text.starts_with("\r\n")) {
      text.erase(0, 2);
    } else if (text.starts_with("\n")) {
      text.erase(0, 1);
    }
    while (text.ends_with('\n') || text.ends_with('\r')) text.pop_back();
    block.code = util::replace_all(text, "\r\n", "\n");
    const std::string data_lang(dom_.attribute(id, "data-lang"));
    block.language = data_lang.empty() ? language_from_classes(classes) : data_lang;
    if (block.language == "math" || has_class(dom_, id, "math")) {
      block.math = ir::MathMode::Display;
      block.language.clear();
      block.code = strip_math_delimiters(block.code);
    }
    out.push_back(std::move(block));
  }

  void convert_figure(NodeId id, ir::Blocks& out, int depth) {
    // A figure with one picture becomes an ImageBlock with the caption.
    NodeId image = html::no_node;
    NodeId caption = html::no_node;
    int pictures = 0;
    std::vector<NodeId> stack{id};
    while (!stack.empty()) {
      const NodeId k = stack.back();
      stack.pop_back();
      const Node& n = dom_.node(k);
      if (n.type != NodeType::Element) continue;
      if (n.name == "img" || n.name == "svg") {
        ++pictures;
        if (image == html::no_node) image = k;
        continue;
      }
      if (n.name == "figcaption" && caption == html::no_node) caption = k;
      if (n.name == "table" || n.name == "pre") pictures += 2;  // not a picture figure
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.push_back(*it);
    }
    if (pictures == 1 && image != html::no_node) {
      ir::Inlines tmp;
      convert_inline(image, tmp, depth);
      if (tmp.size() == 1) {
        if (auto* pic = tmp.front().get_if<ir::InlineImage>()) {
          ir::ImageBlock block{std::move(pic->source), std::move(pic->alt), {}, std::move(pic->title)};
          if (caption != html::no_node) block.caption = collapse_whitespace(dom_.text_content(caption));
          out.push_back(std::move(block));
          return;
        }
      }
    }
    ir::Blocks inner;
    convert_children(id, inner, depth + 1);
    for (ir::Block& b : inner) out.push_back(std::move(b));
  }

  // ---- tables -------------------------------------------------------------------

  void convert_table(NodeId id, ir::Blocks& out, int depth) {
    ir::TableBlock table;
    std::vector<std::pair<NodeId, bool>> rows;  // (tr, in thead)
    std::vector<NodeId> stack;
    const auto visit = [&](auto&& self, NodeId parent, bool head) -> void {
      for (const NodeId child : dom_.node(parent).children) {
        const Node& c = dom_.node(child);
        if (c.type != NodeType::Element) continue;
        if (c.name == "tr") {
          rows.emplace_back(child, head);
        } else if (c.name == "thead") {
          self(self, child, true);
        } else if (c.name == "tbody" || c.name == "tfoot") {
          self(self, child, false);
        } else if (c.name == "caption") {
          convert_inline_children(child, table.caption, depth);
          finish_inlines(table.caption);
        } else if (c.name == "colgroup") {
          for (const NodeId col : c.children) {
            if (dom_.node(col).name == "col") table.alignments.push_back(alignment_of(dom_, col));
          }
        } else if (c.name == "col") {
          table.alignments.push_back(alignment_of(dom_, child));
        }
      }
    };
    visit(visit, id, false);
    for (const auto& [tr, head] : rows) {
      ir::TableRow row;
      bool all_th = true;
      for (const NodeId cell_id : dom_.node(tr).children) {
        const Node& c = dom_.node(cell_id);
        if (c.type != NodeType::Element || (c.name != "td" && c.name != "th")) continue;
        if (c.name != "th") all_th = false;
        ir::TableCell cell;
        cell.colspan = int_attribute(dom_, cell_id, "colspan", 1, 1, 1000);
        cell.rowspan = int_attribute(dom_, cell_id, "rowspan", 1, 1, 1000);
        if (cell.rowspan == 0) cell.rowspan = 1;
        cell.align = alignment_of(dom_, cell_id);
        cell.content = cell_inlines(cell_id, depth);
        row.cells.push_back(std::move(cell));
      }
      if (row.cells.empty()) continue;
      row.header = head || all_th;
      table.rows.push_back(std::move(row));
    }
    // Header rows must lead the table to count as such.
    bool leading = true;
    for (ir::TableRow& row : table.rows) {
      if (!row.header) leading = false;
      if (!leading) row.header = false;
    }
    // Column alignment from the first row's cells when uniform per column.
    if (table.alignments.empty()) {
      const TableGrid grid = table_grid(table);
      std::vector<ir::Alignment> columns(grid.columns, ir::Alignment::Default);
      bool any = false;
      for (std::size_t c = 0; c < grid.columns; ++c) {
        std::optional<ir::Alignment> common;
        for (std::size_t r = 0; r < grid.cells.size(); ++r) {
          const ir::TableCell* cell = grid.cells[r][c];
          if (cell == nullptr) continue;
          if (!common) common = cell->align;
          if (*common != cell->align) common = ir::Alignment::Default;
        }
        if (common && *common != ir::Alignment::Default) {
          columns[c] = *common;
          any = true;
        }
      }
      if (any) {
        table.alignments = columns;
        for (ir::TableRow& row : table.rows) {
          for (ir::TableCell& cell : row.cells) cell.align = ir::Alignment::Default;
        }
      }
    }
    if (table.rows.empty()) return;
    out.push_back(std::move(table));
  }

  // Cells hold inline content: block children are joined with line breaks.
  ir::Inlines cell_inlines(NodeId cell, int depth) {
    ir::Blocks blocks;
    convert_children(cell, blocks, depth + 1);
    ir::Inlines out;
    bool first = true;
    const auto add_block = [&](auto&& self, ir::Block& block) -> void {
      if (!first) out.push_back(ir::LineBreak{});
      first = false;
      if (auto* p = block.get_if<ir::Paragraph>()) {
        for (ir::Inline& i : p->content) out.push_back(std::move(i));
      } else if (auto* h = block.get_if<ir::Heading>()) {
        out.push_back(ir::Strong{std::move(h->content)});
      } else if (auto* code = block.get_if<ir::CodeBlock>()) {
        out.push_back(ir::Code{code->code});
      } else if (auto* image = block.get_if<ir::ImageBlock>()) {
        out.push_back(ir::InlineImage{std::move(image->source), std::move(image->alt)});
      } else if (auto* list = block.get_if<ir::List>()) {
        first = true;
        for (ir::Blocks& item : list->items) {
          for (ir::Block& b : item) self(self, b);
        }
      } else if (auto* quote = block.get_if<ir::BlockQuote>()) {
        first = true;
        for (ir::Block& b : quote->blocks) self(self, b);
      } else if (auto* inner = block.get_if<ir::TableBlock>()) {
        // Nested table: rows as lines, cells separated by " | ".
        bool first_row = true;
        for (ir::TableRow& row : inner->rows) {
          if (!first_row) out.push_back(ir::LineBreak{});
          first_row = false;
          for (std::size_t c = 0; c < row.cells.size(); ++c) {
            if (c > 0) append_text(out, " | ");
            for (ir::Inline& i : row.cells[c].content) out.push_back(std::move(i));
          }
        }
      }
    };
    for (ir::Block& block : blocks) add_block(add_block, block);
    finish_inlines(out);
    return out;
  }

  // ---- inlines --------------------------------------------------------------------

  void convert_inline_children(NodeId id, ir::Inlines& out, int depth) {
    for (const NodeId child : dom_.node(id).children) {
      const Node& c = dom_.node(child);
      if (c.type == NodeType::Text) {
        append_text(out, c.text);
      } else if (c.type == NodeType::Element) {
        if (is_block_element(c.name) && c.name != "br") {
          // Block inside inline context (headings, cells): flatten with breaks.
          ir::Blocks blocks;
          ir::Inlines pending;
          convert_node(child, blocks, pending, depth + 1);
          flush(pending, blocks);
          for (ir::Block& b : blocks) {
            if (auto* p = b.get_if<ir::Paragraph>()) {
              if (!out.empty()) out.push_back(ir::LineBreak{});
              for (ir::Inline& i : p->content) out.push_back(std::move(i));
            } else if (auto* img = b.get_if<ir::ImageBlock>()) {
              out.push_back(ir::InlineImage{std::move(img->source), std::move(img->alt)});
            } else {
              const std::string text = ir::plain_text(ir::Blocks{std::move(b)});
              append_text(out, text);
            }
          }
        } else {
          convert_inline(child, out, depth + 1);
        }
      }
    }
  }

  void wrap(ir::Inlines& out, ir::Inline node, ir::Attributes attr = {}) {
    node.attr = std::move(attr);
    out.push_back(std::move(node));
  }

  void convert_inline(NodeId id, ir::Inlines& out, int depth) {
    const Node& n = dom_.node(id);
    if (n.type == NodeType::Text) {
      append_text(out, n.text);
      return;
    }
    if (n.type != NodeType::Element) return;
    if (depth > ir::max_document_depth - 2) {
      append_text(out, dom_.text_content(id));
      return;
    }
    const std::string& name = n.name;
    ir::Inlines children;
    const auto kids = [&] { convert_inline_children(id, children, depth); };
    if (name == "br") {
      out.push_back(ir::LineBreak{});
    } else if (name == "wbr" || skipped_inputs_.contains(id)) {
      // nothing
    } else if (name == "strong" || name == "b") {
      kids();
      out.push_back(ir::Strong{std::move(children)});
    } else if (name == "em" || name == "i" || name == "cite" || name == "dfn" || name == "var") {
      kids();
      out.push_back(ir::Emphasis{std::move(children)});
    } else if (name == "u" || name == "ins") {
      kids();
      out.push_back(ir::Underline{std::move(children)});
    } else if (name == "s" || name == "strike" || name == "del") {
      kids();
      out.push_back(ir::Strikethrough{std::move(children)});
    } else if (name == "sup") {
      // <sup><a href="#fn1">1</a></sup>: the note reference wins.
      if (n.children.size() == 1 && dom_.node(n.children[0]).name == "a" && note_target(n.children[0])) {
        convert_inline(n.children[0], out, depth + 1);
        return;
      }
      kids();
      out.push_back(ir::Superscript{std::move(children)});
    } else if (name == "sub") {
      kids();
      out.push_back(ir::Subscript{std::move(children)});
    } else if (name == "mark") {
      kids();
      ir::Attributes attr;
      attr.add_class("mark");
      wrap(out, ir::Strong{std::move(children)}, std::move(attr));
    } else if (name == "code" || name == "kbd" || name == "samp" || name == "tt") {
      if (has_class(dom_, id, "math")) {
        ir::Code math{strip_math_delimiters(dom_.text_content(id))};
        math.math = ir::MathMode::Inline;
        out.push_back(std::move(math));
      } else {
        out.push_back(ir::Code{dom_.text_content(id)});
      }
    } else if (name == "q") {
      kids();
      append_text(out, "\xE2\x80\x9C");
      for (ir::Inline& i : children) out.push_back(std::move(i));
      append_text(out, "\xE2\x80\x9D");
    } else if (name == "a") {
      convert_link(id, out, depth);
    } else if (name == "img") {
      convert_image(id, out);
    } else if (name == "svg") {
      ir::EncodedImage svg;
      std::string markup = html::serialize(dom_, id);
      if (markup.find("xmlns") == std::string::npos) {
        markup.insert(4, " xmlns=\"http://www.w3.org/2000/svg\"");
      }
      svg.bytes = to_bytes(markup);
      svg.mime = "image/svg+xml";
      out.push_back(ir::InlineImage{std::move(svg), std::string(dom_.attribute(id, "aria-label"))});
    } else if (name == "math") {
      // MathML: use a TeX annotation when present, else the text.
      std::string tex;
      std::vector<NodeId> stack{id};
      while (!stack.empty()) {
        const NodeId k = stack.back();
        stack.pop_back();
        if (dom_.node(k).name == "annotation" && dom_.attribute(k, "encoding").find("tex") != std::string_view::npos) {
          tex = dom_.text_content(k);
          break;
        }
        for (const NodeId c : dom_.node(k).children) stack.push_back(c);
      }
      if (tex.empty()) {
        std::string alt(dom_.attribute(id, "alttext"));
        if (!alt.empty()) tex = alt;
      }
      if (!tex.empty()) {
        ir::Code math{std::string(util::trim(tex))};
        math.math = dom_.attribute(id, "display") == "block" ? ir::MathMode::Display : ir::MathMode::Inline;
        out.push_back(std::move(math));
      } else {
        append_text(out, collapse_whitespace(dom_.text_content(id)));
      }
    } else if (name == "input") {
      const std::string type = lower_copy(dom_.attribute(id, "type"));
      if (type == "checkbox") {
        append_text(out, dom_.has_attribute(id, "checked") ? "\xE2\x98\x92 " : "\xE2\x98\x90 ");
      } else if (!dom_.attribute(id, "value").empty() && type != "hidden" && type != "password") {
        append_text(out, std::string(dom_.attribute(id, "value")));
      }
    } else if (name == "textarea" || name == "button" || name == "label" || name == "option") {
      kids();
      for (ir::Inline& i : children) out.push_back(std::move(i));
    } else if (name == "video" || name == "audio" || name == "iframe" || name == "object" || name == "embed" ||
               name == "canvas") {
      ++dropped_media_;
    } else {
      // span, font, small, big, abbr, time, bdi, bdo, ruby and unknown elements.
      if (name == "rp") return;  // ruby fallback parentheses
      if (has_class(dom_, id, "math")) {
        ir::Code math{strip_math_delimiters(dom_.text_content(id))};
        math.math = has_class(dom_, id, "display") ? ir::MathMode::Display : ir::MathMode::Inline;
        out.push_back(std::move(math));
        return;
      }
      kids();
      // Presentational CSS.
      const std::string weight = style_value(dom_, id, "font-weight");
      const std::string font_style = style_value(dom_, id, "font-style");
      const std::string decoration =
          style_value(dom_, id, "text-decoration") + " " + style_value(dom_, id, "text-decoration-line");
      const std::string vertical = style_value(dom_, id, "vertical-align");
      ir::Inlines content = std::move(children);
      if (weight == "bold" || weight == "bolder" || weight == "700" || weight == "800" || weight == "900" ||
          has_class(dom_, id, "bold")) {
        content = {ir::Strong{std::move(content)}};
      }
      if (font_style == "italic" || font_style == "oblique" || has_class(dom_, id, "italic")) {
        content = {ir::Emphasis{std::move(content)}};
      }
      if (decoration.find("underline") != std::string::npos || has_class(dom_, id, "underline")) {
        content = {ir::Underline{std::move(content)}};
      }
      if (decoration.find("line-through") != std::string::npos || has_class(dom_, id, "strikethrough")) {
        content = {ir::Strikethrough{std::move(content)}};
      }
      if (vertical == "super") content = {ir::Superscript{std::move(content)}};
      if (vertical == "sub") content = {ir::Subscript{std::move(content)}};
      if (has_class(dom_, id, "mark") || has_class(dom_, id, "highlight")) {
        ir::Attributes attr;
        attr.add_class("mark");
        content = {ir::Inline(ir::Strong{std::move(content)}, std::move(attr))};
      }
      for (ir::Inline& i : content) out.push_back(std::move(i));
    }
  }

  void convert_link(NodeId id, ir::Inlines& out, int depth) {
    if (in_note_ && is_back_link(id)) return;
    if (const auto target = note_target(id)) {
      referenced_.insert(*target);
      ir::Link link;
      link.note = *target;
      link.url = "#fn-" + *target;
      link.children = {ir::Text{collapse_whitespace(dom_.text_content(id))}};
      out.push_back(std::move(link));
      return;
    }
    ir::Inlines children;
    convert_inline_children(id, children, depth);
    if (!dom_.has_attribute(id, "href")) {
      for (ir::Inline& i : children) out.push_back(std::move(i));  // a plain anchor
      return;
    }
    ir::Link link;
    link.url = std::string(util::trim(dom_.attribute(id, "href")));
    link.title = std::string(dom_.attribute(id, "title"));
    link.children = std::move(children);
    out.push_back(std::move(link));
  }

  void convert_image(NodeId id, ir::Inlines& out) {
    std::string src(util::trim(dom_.attribute(id, "src")));
    if (src.empty()) src = std::string(util::trim(dom_.attribute(id, "data-src")));
    ir::InlineImage image;
    image.alt = std::string(dom_.attribute(id, "alt"));
    image.title = std::string(dom_.attribute(id, "title"));
    if (auto embedded = parse_data_uri_image(src)) {
      image.source = std::move(*embedded);
    } else {
      std::optional<ir::EncodedImage> resolved;
      if (options_.resolve && !src.empty()) resolved = options_.resolve(src);
      if (resolved) {
        image.source = std::move(*resolved);
      } else {
        ir::EncodedImage reference;
        reference.url = src;
        image.source = std::move(reference);
      }
    }
    if (src.empty() && image.alt.empty()) return;
    out.push_back(std::move(image));
  }

  // Collapse whitespace the way a browser lays it out: runs become one space,
  // spaces at the start/end and around line breaks disappear.
  void finish_inlines(ir::Inlines& inlines) {
    bool last_space = true;
    collapse(inlines, last_space, 0);
    trim_end(inlines, 0);
    normalize_inlines(inlines);
  }

  static void collapse(ir::Inlines& inlines, bool& last_space, int depth) {
    if (depth > ir::max_document_depth * 2) return;
    for (ir::Inline& item : inlines) {
      std::visit(
          [&](auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            if constexpr (std::same_as<T, ir::Text>) {
              std::string out;
              out.reserve(node.text.size());
              for (const char c : node.text) {
                if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') {
                  if (!last_space) out.push_back(' ');
                  last_space = true;
                } else {
                  out.push_back(c);
                  last_space = false;
                }
              }
              node.text = std::move(out);
            } else if constexpr (std::same_as<T, ir::LineBreak>) {
              last_space = true;
            } else if constexpr (std::same_as<T, ir::Code> || std::same_as<T, ir::InlineImage>) {
              last_space = false;
            } else {
              collapse(node.children, last_space, depth + 1);
            }
          },
          item.node);
    }
  }

  // Remove trailing spaces of the run and spaces before line breaks.
  static bool trim_end(ir::Inlines& inlines, int depth) {
    if (depth > ir::max_document_depth * 2) return true;
    for (auto it = inlines.rbegin(); it != inlines.rend(); ++it) {
      bool done = true;
      std::visit(
          [&](auto& node) {
            using T = std::remove_cvref_t<decltype(node)>;
            if constexpr (std::same_as<T, ir::Text>) {
              while (!node.text.empty() && node.text.back() == ' ') node.text.pop_back();
              done = !node.text.empty();
            } else if constexpr (std::same_as<T, ir::LineBreak> || std::same_as<T, ir::Code> ||
                                 std::same_as<T, ir::InlineImage>) {
              done = true;
            } else {
              done = trim_end(node.children, depth + 1);
            }
          },
          item_node(*it));
      if (done) break;
    }
    // Spaces just before a line break.
    for (std::size_t i = 1; i < inlines.size(); ++i) {
      if (inlines[i].is<ir::LineBreak>()) {
        if (auto* t = inlines[i - 1].get_if<ir::Text>()) {
          while (!t->text.empty() && t->text.back() == ' ') t->text.pop_back();
        }
      }
    }
    return !inlines.empty();
  }

  static ir::Inline::Node& item_node(ir::Inline& item) { return item.node; }

  const Dom& dom_;
  const HtmlReadOptions& options_;
  WarningSink warnings_;
  ir::Document document_;
  std::set<NodeId> notes_sections_;
  std::vector<std::pair<std::string, NodeId>> note_elements_;
  std::map<std::string, std::size_t> note_ids_;
  std::set<std::string> referenced_;
  std::set<NodeId> skipped_inputs_;
  bool in_note_ = false;
  int dropped_media_ = 0;
};

Result<ir::Document> convert_dom(const Dom& dom, const HtmlReadOptions& options, WarningSink warnings) {
  if (warnings != nullptr) {
    for (const std::string& w : dom.warnings) warnings->push_back(w);
  }
  Converter converter(dom, options, warnings);
  ir::Document document = converter.run();
  enforce_depth(document);
  return document;
}

bool looks_like_xhtml(std::string_view text) {
  const std::string_view head = util::trim_left(text.substr(0, 512));
  return head.starts_with("<?xml") || text.substr(0, 2048).find("xmlns=\"http://www.w3.org/1999/xhtml\"") != std::string_view::npos;
}

}  // namespace

Result<ir::Document> parse_html_text(std::string_view utf8, const HtmlReadOptions& options, WarningSink warnings) {
  html::ParseSettings settings;
  settings.xhtml = options.xhtml || looks_like_xhtml(utf8);
  settings.max_open_elements = std::clamp<std::size_t>(options.max_open_elements, 8, 4096);
  const Dom dom = html::parse_dom(utf8, settings);
  return convert_dom(dom, options, warnings);
}

Result<ir::Document> parse_html(ByteView data, const HtmlReadOptions& options, WarningSink warnings) {
  std::string label = options.forced_encoding;
  if (label.empty()) label = html::prescan_charset(data);
  std::string text;
  bool decoded = false;
  if (!label.empty()) {
    // A page claiming UTF-16 in a <meta> while being ASCII-compatible is UTF-8 (WHATWG).
    const std::string normalized = util::to_lower(label);
    if (normalized.starts_with("utf-16") && !util::detect_bom(data) && data.size() > 1 &&
        data[0] != std::byte{0} && data[1] != std::byte{0}) {
      label = "utf-8";
    }
    decoded = decode_charset(data, label, text);
    if (!decoded && warnings != nullptr) {
      warnings->push_back("Unsupported character set '" + label + "'; the page was read as UTF-8/Windows-1252.");
    }
  }
  if (!decoded) {
    const std::string_view raw = as_chars(data);
    if (util::is_valid_utf8(raw)) {
      text = std::string(raw);
    } else {
      decode_charset(data, options.fallback_encoding.empty() ? "windows-1252" : options.fallback_encoding, text);
    }
  } else if (util::detect_bom(data) && text.starts_with("\xEF\xBB\xBF")) {
    text.erase(0, 3);
  }
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  std::erase(text, '\0');
  if (!util::is_valid_utf8(text)) text = util::sanitize_utf8(text);
  return parse_html_text(text, options, warnings);
}

}  // namespace cvt::document
