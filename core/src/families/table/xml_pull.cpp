// Convertible core - table family: minimal streaming XML reader.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/table/xml_pull.hpp"

#include <cstdint>
#include <string>

#include "util/text.hpp"

namespace cvt::table {

namespace {

bool is_name_char(char c) noexcept {
  return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '>' || c == '=' || c == '<' ||
           c == '"' || c == '\'');
}

}  // namespace

void xml_decode_append(std::string& out, std::string_view raw) {
  std::size_t i = 0;
  while (i < raw.size()) {
    const std::size_t amp = raw.find('&', i);
    if (amp == std::string_view::npos) {
      out.append(raw.substr(i));
      return;
    }
    out.append(raw.substr(i, amp - i));
    const std::size_t semi = raw.find(';', amp + 1);
    if (semi == std::string_view::npos || semi - amp > 12) {
      out.push_back('&');
      i = amp + 1;
      continue;
    }
    const std::string_view entity = raw.substr(amp + 1, semi - amp - 1);
    if (entity == "lt") {
      out.push_back('<');
    } else if (entity == "gt") {
      out.push_back('>');
    } else if (entity == "amp") {
      out.push_back('&');
    } else if (entity == "quot") {
      out.push_back('"');
    } else if (entity == "apos") {
      out.push_back('\'');
    } else if (entity.size() > 1 && entity[0] == '#') {
      std::optional<std::int64_t> code;
      if (entity[1] == 'x' || entity[1] == 'X') {
        code = util::parse_int(entity.substr(2), 16);
      } else {
        code = util::parse_int(entity.substr(1), 10);
      }
      if (code && *code > 0 && *code <= 0x10FFFF) {
        util::append_utf8(out, static_cast<char32_t>(*code));
      } else {
        out.append(raw.substr(amp, semi - amp + 1));
      }
    } else {
      out.append(raw.substr(amp, semi - amp + 1));
    }
    i = semi + 1;
  }
}

namespace {

// XML end-of-line handling: CRLF and lone CR read as LF.
std::string normalize_newlines(std::string text) {
  if (text.find('\r') == std::string::npos) return text;
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      out.push_back('\n');
      if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

}  // namespace

std::string xml_text(ByteView data) {
  if (const auto bom = util::detect_bom(data); bom && bom->encoding != util::TextEncoding::Utf8) {
    return normalize_newlines(util::decode_text(data, bom->encoding).text);
  }
  std::string_view text = as_chars(data);
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  // BOM-less UTF-16 starts with "<\0" or "\0<".
  if (text.size() >= 2 && text[0] == '<' && text[1] == '\0') {
    return normalize_newlines(util::decode_text(data, util::TextEncoding::Utf16LE).text);
  }
  if (text.size() >= 2 && text[0] == '\0' && text[1] == '<') {
    return normalize_newlines(util::decode_text(data, util::TextEncoding::Utf16BE).text);
  }
  return normalize_newlines(std::string(text));
}

void xml_escape_append(std::string& out, std::string_view text, bool attribute) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      switch (c) {
        case '&': out.append("&amp;"); break;
        case '<': out.append("&lt;"); break;
        case '>': out.append("&gt;"); break;
        case '"':
          if (attribute) {
            out.append("&quot;");
          } else {
            out.push_back('"');
          }
          break;
        case '\n':
          if (attribute) {
            out.append("&#10;");
          } else {
            out.push_back('\n');
          }
          break;
        case '\r': out.append("&#13;"); break;
        case '\t':
          if (attribute) {
            out.append("&#9;");
          } else {
            out.push_back('\t');
          }
          break;
        default:
          if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));
          break;
      }
      ++i;
      continue;
    }
    const util::Utf8Decoded d = util::decode_utf8(text, i);
    if (d.ok && d.code_point != 0xFFFE && d.code_point != 0xFFFF) {
      out.append(text.substr(i, d.length));
    } else {
      util::append_utf8(out, util::replacement_character);
    }
    i += d.length == 0 ? 1 : d.length;
  }
}

XmlPull::XmlPull(std::string_view xml, std::size_t max_depth) noexcept : xml_(xml), max_depth_(max_depth) {}

std::string_view XmlPull::local_name() const noexcept {
  const std::size_t colon = name_.rfind(':');
  return colon == std::string_view::npos ? name_ : name_.substr(colon + 1);
}

std::optional<std::string> XmlPull::attribute(std::string_view name) const {
  const auto decode = [](std::string_view raw) {
    std::string out;
    xml_decode_append(out, raw);
    return out;
  };
  for (const auto& [key, value] : attributes_) {
    if (key == name) return decode(value);
  }
  for (const auto& [key, value] : attributes_) {
    const std::size_t colon = key.rfind(':');
    if (colon != std::string_view::npos && key.substr(colon + 1) == name) return decode(value);
  }
  return std::nullopt;
}

Result<XmlPull::Event> XmlPull::next() {
  if (pending_end_) {
    pending_end_ = false;
    --depth_;
    return Event::EndElement;
  }
  while (pos_ < xml_.size()) {
    if (xml_[pos_] != '<') {
      const std::size_t lt = xml_.find('<', pos_);
      const std::string_view raw = xml_.substr(pos_, lt == std::string_view::npos ? std::string_view::npos : lt - pos_);
      pos_ = lt == std::string_view::npos ? xml_.size() : lt;
      if (depth_ == 0) continue;  // whitespace outside the document element
      text_.clear();
      xml_decode_append(text_, raw);
      return Event::Text;
    }
    const std::string_view rest = xml_.substr(pos_);
    if (rest.starts_with("<?")) {
      const std::size_t end = xml_.find("?>", pos_ + 2);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated processing instruction in XML");
      pos_ = end + 2;
      continue;
    }
    if (rest.starts_with("<!--")) {
      const std::size_t end = xml_.find("-->", pos_ + 4);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated comment in XML");
      pos_ = end + 3;
      continue;
    }
    if (rest.starts_with("<![CDATA[")) {
      const std::size_t end = xml_.find("]]>", pos_ + 9);
      if (end == std::string_view::npos) return fail(Status::Decode, "unterminated CDATA section in XML");
      text_.assign(xml_.substr(pos_ + 9, end - pos_ - 9));
      pos_ = end + 3;
      return Event::Text;
    }
    if (rest.starts_with("<!")) {
      // DOCTYPE (possibly with an internal subset in brackets): skipped.
      int brackets = 0;
      std::size_t i = pos_ + 2;
      for (; i < xml_.size(); ++i) {
        const char c = xml_[i];
        if (c == '[') {
          ++brackets;
        } else if (c == ']') {
          --brackets;
        } else if (c == '>' && brackets <= 0) {
          break;
        }
      }
      if (i >= xml_.size()) return fail(Status::Decode, "unterminated declaration in XML");
      pos_ = i + 1;
      continue;
    }
    return read_tag();
  }
  if (depth_ != 0) return fail(Status::Decode, "the XML document ends inside an element");
  return Event::End;
}

Result<XmlPull::Event> XmlPull::read_tag() {
  std::size_t i = pos_ + 1;
  const bool closing = i < xml_.size() && xml_[i] == '/';
  if (closing) ++i;
  const std::size_t name_start = i;
  while (i < xml_.size() && is_name_char(xml_[i])) ++i;
  if (i == name_start) return fail(Status::Decode, "malformed XML tag at byte {}", pos_);
  name_ = xml_.substr(name_start, i - name_start);
  if (closing) {
    while (i < xml_.size() && util::is_ascii_space(xml_[i])) ++i;
    if (i >= xml_.size() || xml_[i] != '>') return fail(Status::Decode, "malformed XML end tag at byte {}", pos_);
    pos_ = i + 1;
    if (depth_ == 0) return fail(Status::Decode, "unbalanced XML end tag at byte {}", pos_);
    --depth_;
    return Event::EndElement;
  }
  attributes_.clear();
  while (true) {
    while (i < xml_.size() && util::is_ascii_space(xml_[i])) ++i;
    if (i >= xml_.size()) return fail(Status::Decode, "unterminated XML tag");
    if (xml_[i] == '>') {
      pos_ = i + 1;
      break;
    }
    if (xml_[i] == '/' && i + 1 < xml_.size() && xml_[i + 1] == '>') {
      pos_ = i + 2;
      pending_end_ = true;
      break;
    }
    const std::size_t key_start = i;
    while (i < xml_.size() && is_name_char(xml_[i])) ++i;
    if (i == key_start) return fail(Status::Decode, "malformed XML attribute at byte {}", i);
    const std::string_view key = xml_.substr(key_start, i - key_start);
    while (i < xml_.size() && util::is_ascii_space(xml_[i])) ++i;
    if (i >= xml_.size() || xml_[i] != '=') return fail(Status::Decode, "XML attribute without a value at byte {}", i);
    ++i;
    while (i < xml_.size() && util::is_ascii_space(xml_[i])) ++i;
    if (i >= xml_.size() || (xml_[i] != '"' && xml_[i] != '\'')) {
      return fail(Status::Decode, "unquoted XML attribute value at byte {}", i);
    }
    const char quote = xml_[i];
    const std::size_t close = xml_.find(quote, i + 1);
    if (close == std::string_view::npos) return fail(Status::Decode, "unterminated XML attribute value");
    attributes_.emplace_back(key, xml_.substr(i + 1, close - i - 1));
    i = close + 1;
  }
  if (++depth_ > max_depth_) return fail(Status::Limit, "XML elements are nested more than {} levels deep", max_depth_);
  return Event::StartElement;
}

Result<void> XmlPull::skip_element() {
  const std::size_t target = depth_ - 1;
  while (depth_ > target) {
    CVT_TRY_ASSIGN(const Event event, next());
    if (event == Event::End) return fail(Status::Decode, "the XML document ends inside an element");
  }
  return {};
}

Result<std::string> XmlPull::element_text() {
  std::string out;
  const std::size_t target = depth_ - 1;
  while (depth_ > target) {
    CVT_TRY_ASSIGN(const Event event, next());
    if (event == Event::Text) out.append(text_);
    if (event == Event::End) return fail(Status::Decode, "the XML document ends inside an element");
  }
  return out;
}

}  // namespace cvt::table
