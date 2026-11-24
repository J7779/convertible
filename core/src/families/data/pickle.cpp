// Convertible core - data family: Python pickles (protocols 0 to 5), read only,
// and strictly as data.
//
// Nothing is ever executed or imported: only the opcodes that build plain
// values are interpreted - None, booleans, integers of any size, floats,
// str, bytes and bytearray (-> $binary), lists, tuples, dicts, sets and
// frozensets, plus the memo (shared references are copied; reference cycles
// are an error). Older protocols spell bytes and sets as calls of a few
// built-in constructors (_codecs.encode, set, frozenset, bytes, bytearray);
// those exact calls are recognised and rebuilt as data - nothing is looked
// up or called. Any other global, and every opcode that would build an
// object or fetch an external buffer (BUILD, INST, OBJ, NEWOBJ, PERSID, EXT*,
// NEXT_BUFFER, ...), stops the read with an error naming it.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/registry.hpp"
#include "families/data/binary_common.hpp"
#include "families/data/data.hpp"
#include "util/bytes.hpp"

namespace cvt::data {

namespace {

struct Node {
  enum class Kind : std::uint8_t { Scalar, List, Tuple, Dict, Set, Mark, Callable };
  Kind kind = Kind::Scalar;
  Json scalar;
  std::vector<std::shared_ptr<Node>> items;
  std::vector<std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>>> entries;
};
using NodePtr = std::shared_ptr<Node>;

NodePtr scalar_node(Json value) {
  auto node = std::make_shared<Node>();
  node->scalar = std::move(value);
  return node;
}

NodePtr container_node(Node::Kind kind) {
  auto node = std::make_shared<Node>();
  node->kind = kind;
  return node;
}

std::string_view forbidden_name(std::uint8_t op) {
  switch (op) {
    case 'b': return "BUILD";
    case 'i': return "INST";
    case 'o': return "OBJ";
    case 0x81: return "NEWOBJ";
    case 0x92: return "NEWOBJ_EX";
    case 'P': return "PERSID";
    case 'Q': return "BINPERSID";
    case 0x82:
    case 0x83:
    case 0x84: return "EXT";
    case 0x97: return "NEXT_BUFFER";
    case 0x98: return "READONLY_BUFFER";
    default: return {};
  }
}

class PickleReader {
 public:
  PickleReader(ByteView data, const ReadContext& ctx)
      : reader_(data), ctx_(ctx), policy_(read_policy(ctx.options)), budget_(data.size()), output_budget_(data.size()) {}

  Result<Json> parse() {
    while (true) {
      if (reader_.at_end()) return fail(Status::Decode, "the pickle ends without a STOP opcode");
      const std::size_t at = reader_.position();
      const std::uint8_t op = reader_.u8();
      if (const std::string_view name = forbidden_name(op); !name.empty()) {
        return fail(Status::Decode, "the pickle uses the {} opcode at byte {}, which would import or call Python "
                    "code; only plain data (lists, dicts, numbers, strings, bytes) is read", name, at);
      }
      if (op == '.') break;
      CVT_TRY(step(op, at));
      if ((++ops_ & 4095) == 0) CVT_TRY(ctx_.checkpoint(static_cast<double>(at) / static_cast<double>(reader_.size())));
    }
    if (stack_.size() != 1) return fail(Status::Decode, "the pickle leaves {} values on its stack", stack_.size());
    std::vector<const Node*> open;
    return to_json(*stack_.back(), open, 0);
  }

 private:
  std::unexpected<Error> bad(std::size_t at, std::string_view what) const {
    return fail(Status::Decode, "invalid pickle at byte {}: {}", at, what);
  }

  Result<ByteView> take(std::uint64_t size, std::size_t at) {
    if (size > reader_.remaining()) return bad(at, "truncated data");
    return reader_.bytes(static_cast<std::size_t>(size));
  }

  Result<std::string_view> line(std::size_t at) {
    const std::string_view rest = as_chars(reader_.peek_rest());
    const std::size_t end = rest.find('\n');
    if (end == std::string_view::npos) return bad(at, "unterminated text argument");
    (void)reader_.bytes(end + 1);
    std::string_view text = rest.substr(0, end);
    if (text.ends_with('\r')) text.remove_suffix(1);
    return text;
  }

  Result<NodePtr> pop(std::size_t at) {
    if (stack_.empty() || stack_.back()->kind == Node::Kind::Mark) return bad(at, "stack underflow");
    NodePtr top = std::move(stack_.back());
    stack_.pop_back();
    return top;
  }

  Result<std::vector<NodePtr>> pop_mark(std::size_t at) {
    std::size_t mark = stack_.size();
    while (mark > 0 && stack_[mark - 1]->kind != Node::Kind::Mark) --mark;
    if (mark == 0) return bad(at, "missing MARK");
    std::vector<NodePtr> items(std::make_move_iterator(stack_.begin() + static_cast<std::ptrdiff_t>(mark)),
                               std::make_move_iterator(stack_.end()));
    stack_.resize(mark - 1);
    return items;
  }

  Result<void> push(NodePtr node) {
    CVT_TRY(budget_.take());
    stack_.push_back(std::move(node));
    return {};
  }

  /// Python string literal (protocol 0 STRING): quotes and backslash escapes.
  Result<std::string> python_literal(std::string_view text, std::size_t at) {
    if (text.size() < 2 || (text.front() != '\'' && text.front() != '"') || text.back() != text.front()) {
      return bad(at, "malformed STRING argument");
    }
    text = text.substr(1, text.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '\\' || i + 1 >= text.size()) {
        out.push_back(text[i]);
        continue;
      }
      const char e = text[++i];
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'a': out.push_back('\a'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'v': out.push_back('\v'); break;
        case 'x': {
          const auto value = i + 2 < text.size() ? util::parse_int(text.substr(i + 1, 2), 16) : std::nullopt;
          if (!value) return bad(at, "invalid \\x escape");
          out.push_back(static_cast<char>(*value));
          i += 2;
          break;
        }
        default:
          if (e >= '0' && e <= '7') {
            int value = e - '0';
            for (int k = 0; k < 2 && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7'; ++k) value = value * 8 + (text[++i] - '0');
            out.push_back(static_cast<char>(value & 0xFF));
          } else {
            out.push_back(e);
          }
      }
    }
    return out;
  }

  /// raw-unicode-escape: \uXXXX / \UXXXXXXXX, other bytes are Latin-1.
  Result<std::string> raw_unicode(std::string_view text, std::size_t at) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == 'u' || text[i + 1] == 'U')) {
        const std::size_t digits = text[i + 1] == 'u' ? 4 : 8;
        const auto value = i + 2 + digits <= text.size() ? util::parse_int(text.substr(i + 2, digits), 16) : std::nullopt;
        if (!value || !util::is_scalar_value(static_cast<char32_t>(*value))) return bad(at, "invalid \\u escape");
        util::append_utf8(out, static_cast<char32_t>(*value));
        i += 1 + digits;
        continue;
      }
      util::append_utf8(out, static_cast<char32_t>(static_cast<unsigned char>(text[i])));
    }
    return out;
  }

  Json bytes_or_text(ByteView data) {
    // Protocol 0-2 "str" of Python 2: text when it is UTF-8.
    const std::string_view text = as_chars(data);
    if (util::is_valid_utf8(text)) return Json(std::string(text));
    return make_binary(data);
  }

  Result<void> memo_put(std::uint64_t index, std::size_t at) {
    if (stack_.empty() || stack_.back()->kind == Node::Kind::Mark) return bad(at, "PUT with an empty stack");
    if (memo_.size() > budget_.limit) return fail(Status::Limit, "the pickle memo is too large");
    memo_[index] = stack_.back();
    return {};
  }

  Result<void> memo_get(std::uint64_t index, std::size_t at) {
    const auto found = memo_.find(index);
    if (found == memo_.end()) return bad(at, std::format("memo entry {} is not defined", index));
    return push(found->second);
  }

  /// The few constructors older protocols use for plain data.
  Result<void> global(std::string module, std::string name, std::size_t at) {
    if (module == "__builtin__") module = "builtins";
    const std::string full = module + "." + name;
    static constexpr std::string_view allowed[] = {"_codecs.encode", "builtins.set", "builtins.frozenset",
                                                   "builtins.bytes", "builtins.bytearray"};
    if (std::ranges::find(allowed, full) == std::end(allowed)) {
      return fail(Status::Decode, "the pickle refers to the Python object {} at byte {}; only plain data (lists, dicts, "
                  "numbers, strings, bytes) is read and nothing is ever imported or called", full, at);
    }
    auto node = container_node(Node::Kind::Callable);
    node->scalar = Json(full);
    return push(std::move(node));
  }

  Result<void> reduce(std::size_t at) {
    CVT_TRY_ASSIGN(NodePtr args, pop(at));
    CVT_TRY_ASSIGN(NodePtr callable, pop(at));
    if (callable->kind != Node::Kind::Callable || args->kind != Node::Kind::Tuple) {
      return bad(at, "REDUCE of something that is not data");
    }
    const std::string& name = callable->scalar.as_string();
    const auto& items = args->items;
    const auto latin1 = [](std::string_view text) {
      Bytes out;
      for (const char32_t cp : util::utf8_to_utf32(text)) out.push_back(static_cast<std::byte>(cp & 0xFF));
      return out;
    };
    if (name == "_codecs.encode" && items.size() == 2 && items[0]->kind == Node::Kind::Scalar &&
        items[0]->scalar.is_string()) {
      const std::string encoding = items[1]->scalar.as_string();
      const std::string& text = items[0]->scalar.as_string();
      if (encoding == "latin1" || encoding == "latin-1" || encoding == "iso-8859-1") {
        return push(scalar_node(make_binary(latin1(text))));
      }
      if (encoding == "utf-8" || encoding == "utf8") return push(scalar_node(make_binary(as_bytes(text))));
      return bad(at, "unsupported encoding in a pickled bytes value");
    }
    if ((name == "builtins.set" || name == "builtins.frozenset") && items.size() <= 1) {
      auto node = container_node(Node::Kind::Set);
      if (!items.empty()) {
        if (items[0]->kind != Node::Kind::List && items[0]->kind != Node::Kind::Tuple &&
            items[0]->kind != Node::Kind::Set) {
          return bad(at, "set() of something that is not a list");
        }
        node->items = items[0]->items;
      }
      return push(std::move(node));
    }
    if (name == "builtins.bytes" || name == "builtins.bytearray") {
      if (items.empty()) return push(scalar_node(make_binary(ByteView{})));
      if (items[0]->kind == Node::Kind::Scalar && tag_kind(items[0]->scalar) == TagKind::Binary) return push(items[0]);
      if (items.size() == 2 && items[0]->scalar.is_string()) {
        return push(scalar_node(make_binary(latin1(items[0]->scalar.as_string()))));
      }
      if (items[0]->kind == Node::Kind::List) {
        Bytes out;
        for (const NodePtr& item : items[0]->items) out.push_back(static_cast<std::byte>(item->scalar.as_int64() & 0xFF));
        return push(scalar_node(make_binary(out)));
      }
    }
    return bad(at, std::format("unsupported arguments for {}", name));
  }

  Result<void> step(std::uint8_t op, std::size_t at) {
    const auto need = [&](std::size_t n) -> Result<void> {
      if (n > reader_.remaining()) return bad(at, "truncated argument");
      return {};
    };
    switch (op) {
      case 'c': {
        CVT_TRY_ASSIGN(const std::string_view module, line(at));
        CVT_TRY_ASSIGN(const std::string_view name, line(at));
        return global(std::string(module), std::string(name), at);
      }
      case 0x93: {
        CVT_TRY_ASSIGN(NodePtr name, pop(at));
        CVT_TRY_ASSIGN(NodePtr module, pop(at));
        if (!name->scalar.is_string() || !module->scalar.is_string()) return bad(at, "STACK_GLOBAL needs two strings");
        return global(module->scalar.as_string(), name->scalar.as_string(), at);
      }
      case 'R': return reduce(at);
      case 0x80: CVT_TRY(need(1)); (void)reader_.u8(); return {};  // PROTO
      case 0x95: CVT_TRY(need(8)); (void)reader_.u64le(); return {};  // FRAME
      case '(': stack_.push_back(container_node(Node::Kind::Mark)); return {};
      case '0': {
        if (stack_.empty()) return bad(at, "POP with an empty stack");
        stack_.pop_back();
        return {};
      }
      case '1': {
        CVT_TRY(pop_mark(at));
        return {};
      }
      case '2': {
        if (stack_.empty()) return bad(at, "DUP with an empty stack");
        return push(stack_.back());
      }
      case 'N': return push(scalar_node(Json()));
      case 0x88: return push(scalar_node(Json(true)));
      case 0x89: return push(scalar_node(Json(false)));
      case 'I': {
        CVT_TRY_ASSIGN(const std::string_view text, line(at));
        if (text == "00") return push(scalar_node(Json(false)));
        if (text == "01") return push(scalar_node(Json(true)));
        if (!decimal_parts(text) || text.find_first_of(".eE") != std::string_view::npos) return bad(at, "malformed INT");
        return push(scalar_node(number_from_literal(text)));
      }
      case 'L': {
        CVT_TRY_ASSIGN(std::string_view text, line(at));
        if (text.ends_with('L')) text.remove_suffix(1);
        if (!decimal_parts(text) || text.find_first_of(".eE") != std::string_view::npos) return bad(at, "malformed LONG");
        return push(scalar_node(number_from_literal(text)));
      }
      case 'J': CVT_TRY(need(4)); return push(scalar_node(Json(static_cast<std::int64_t>(reader_.i32le()))));
      case 'K': CVT_TRY(need(1)); return push(scalar_node(Json(static_cast<std::int64_t>(reader_.u8()))));
      case 'M': CVT_TRY(need(2)); return push(scalar_node(Json(static_cast<std::int64_t>(reader_.u16le()))));
      case 0x8A:
      case 0x8B: {
        std::uint64_t size = 0;
        if (op == 0x8A) {
          CVT_TRY(need(1));
          size = reader_.u8();
        } else {
          CVT_TRY(need(4));
          const std::int32_t n = reader_.i32le();
          if (n < 0) return bad(at, "negative LONG4 size");
          size = static_cast<std::uint64_t>(n);
        }
        if (size > 4096) return bad(at, "integer too large");
        CVT_TRY_ASSIGN(const ByteView little, take(size, at));
        Bytes big(little.rbegin(), little.rend());
        return push(scalar_node(number_from_literal(twos_complement_to_decimal(big))));
      }
      case 'F': {
        CVT_TRY_ASSIGN(const std::string_view text, line(at));
        const auto value = util::parse_double(text);
        if (!value) return bad(at, "malformed FLOAT");
        return push(scalar_node(Json(*value)));
      }
      case 'G': CVT_TRY(need(8)); return push(scalar_node(Json(reader_.f64be())));
      case 'S': {
        CVT_TRY_ASSIGN(const std::string_view text, line(at));
        CVT_TRY_ASSIGN(const std::string value, python_literal(text, at));
        return push(scalar_node(bytes_or_text(as_bytes(value))));
      }
      case 'T':
      case 'U': {
        std::uint64_t size = 0;
        if (op == 'U') {
          CVT_TRY(need(1));
          size = reader_.u8();
        } else {
          CVT_TRY(need(4));
          size = reader_.u32le();
        }
        CVT_TRY_ASSIGN(const ByteView data, take(size, at));
        return push(scalar_node(bytes_or_text(data)));
      }
      case 'V': {
        CVT_TRY_ASSIGN(const std::string_view text, line(at));
        CVT_TRY_ASSIGN(std::string value, raw_unicode(text, at));
        return push(scalar_node(Json(std::move(value))));
      }
      case 'X':
      case 0x8C:
      case 0x8D: {
        std::uint64_t size = 0;
        if (op == 0x8C) {
          CVT_TRY(need(1));
          size = reader_.u8();
        } else if (op == 'X') {
          CVT_TRY(need(4));
          size = reader_.u32le();
        } else {
          CVT_TRY(need(8));
          size = reader_.u64le();
        }
        CVT_TRY_ASSIGN(const ByteView data, take(size, at));
        std::size_t repaired = 0;
        return push(scalar_node(text_value(as_chars(data), repaired)));
      }
      case 'B':
      case 'C':
      case 0x8E:
      case 0x96: {
        std::uint64_t size = 0;
        if (op == 'C') {
          CVT_TRY(need(1));
          size = reader_.u8();
        } else if (op == 'B') {
          CVT_TRY(need(4));
          size = reader_.u32le();
        } else {
          CVT_TRY(need(8));
          size = reader_.u64le();
        }
        CVT_TRY_ASSIGN(const ByteView data, take(size, at));
        return push(scalar_node(make_binary(data)));
      }
      case ']': return push(container_node(Node::Kind::List));
      case ')': return push(container_node(Node::Kind::Tuple));
      case '}': return push(container_node(Node::Kind::Dict));
      case 0x8F: return push(container_node(Node::Kind::Set));
      case 'l':
      case 't':
      case 0x91: {
        CVT_TRY_ASSIGN(auto items, pop_mark(at));
        auto node = container_node(op == 'l' ? Node::Kind::List : op == 't' ? Node::Kind::Tuple : Node::Kind::Set);
        node->items = std::move(items);
        return push(std::move(node));
      }
      case 0x85:
      case 0x86:
      case 0x87: {
        const std::size_t count = static_cast<std::size_t>(op - 0x84);
        auto node = container_node(Node::Kind::Tuple);
        node->items.resize(count);
        for (std::size_t i = count; i-- > 0;) {
          CVT_TRY_ASSIGN(node->items[i], pop(at));
        }
        return push(std::move(node));
      }
      case 'd': {
        CVT_TRY_ASSIGN(auto items, pop_mark(at));
        if (items.size() % 2 != 0) return bad(at, "odd number of items for DICT");
        auto node = container_node(Node::Kind::Dict);
        for (std::size_t i = 0; i < items.size(); i += 2) node->entries.emplace_back(items[i], items[i + 1]);
        return push(std::move(node));
      }
      case 'a': {
        CVT_TRY_ASSIGN(NodePtr value, pop(at));
        if (stack_.empty() || stack_.back()->kind != Node::Kind::List) return bad(at, "APPEND without a list");
        CVT_TRY(budget_.take());
        stack_.back()->items.push_back(std::move(value));
        return {};
      }
      case 'e':
      case 0x90: {
        CVT_TRY_ASSIGN(auto items, pop_mark(at));
        const Node::Kind wanted = op == 'e' ? Node::Kind::List : Node::Kind::Set;
        if (stack_.empty() || stack_.back()->kind != wanted) return bad(at, op == 'e' ? "APPENDS without a list" : "ADDITEMS without a set");
        CVT_TRY(budget_.take(items.size()));
        for (NodePtr& item : items) stack_.back()->items.push_back(std::move(item));
        return {};
      }
      case 's': {
        CVT_TRY_ASSIGN(NodePtr value, pop(at));
        CVT_TRY_ASSIGN(NodePtr key, pop(at));
        if (stack_.empty() || stack_.back()->kind != Node::Kind::Dict) return bad(at, "SETITEM without a dict");
        stack_.back()->entries.emplace_back(std::move(key), std::move(value));
        return {};
      }
      case 'u': {
        CVT_TRY_ASSIGN(auto items, pop_mark(at));
        if (items.size() % 2 != 0 || stack_.empty() || stack_.back()->kind != Node::Kind::Dict) {
          return bad(at, "SETITEMS without a dict");
        }
        CVT_TRY(budget_.take(items.size()));
        for (std::size_t i = 0; i < items.size(); i += 2) stack_.back()->entries.emplace_back(items[i], items[i + 1]);
        return {};
      }
      case 'p': {
        CVT_TRY_ASSIGN(const std::string_view text, line(at));
        const auto index = util::parse_int(text);
        if (!index || *index < 0) return bad(at, "malformed PUT");
        return memo_put(static_cast<std::uint64_t>(*index), at);
      }
      case 'q': CVT_TRY(need(1)); return memo_put(reader_.u8(), at);
      case 'r': CVT_TRY(need(4)); return memo_put(reader_.u32le(), at);
      case 0x94: return memo_put(memo_.size(), at);
      case 'g': {
        CVT_TRY_ASSIGN(const std::string_view text, line(at));
        const auto index = util::parse_int(text);
        if (!index || *index < 0) return bad(at, "malformed GET");
        return memo_get(static_cast<std::uint64_t>(*index), at);
      }
      case 'h': CVT_TRY(need(1)); return memo_get(reader_.u8(), at);
      case 'j': CVT_TRY(need(4)); return memo_get(reader_.u32le(), at);
      default: break;
    }
    return bad(at, std::format("unknown opcode 0x{:02X}", op));
  }

  Result<Json> to_json(const Node& node, std::vector<const Node*>& open, std::size_t depth) {
    if (depth > policy_.max_depth) {
      return fail(Status::Limit, "the pickle is nested more than {} levels deep (option max_depth)", policy_.max_depth);
    }
    CVT_TRY(output_budget_.take());
    if (node.kind == Node::Kind::Scalar) return node.scalar;
    if (node.kind == Node::Kind::Mark) return fail(Status::Decode, "a MARK was left on the pickle stack");
    if (node.kind == Node::Kind::Callable) return fail(Status::Decode, "the pickle holds a reference to a Python object");
    if (std::ranges::find(open, &node) != open.end()) {
      return fail(Status::Decode, "the pickle contains a reference cycle (a container that holds itself)");
    }
    open.push_back(&node);
    Json out;
    if (node.kind == Node::Kind::Dict) {
      out = Json::object();
      for (const auto& [key, value] : node.entries) {
        CVT_TRY_ASSIGN(Json k, to_json(*key, open, depth + 1));
        CVT_TRY_ASSIGN(Json v, to_json(*value, open, depth + 1));
        if (!k.is_string()) ++converted_keys_;
        CVT_TRY(put_member(out, key_from_value(k), std::move(v), policy_));
      }
    } else {
      out = Json::array();
      for (const NodePtr& item : node.items) {
        CVT_TRY_ASSIGN(Json v, to_json(*item, open, depth + 1));
        out.push_back(std::move(v));
      }
    }
    open.pop_back();
    return out;
  }

 public:
  std::size_t converted_keys_ = 0;

 private:
  util::ByteReader reader_;
  const ReadContext& ctx_;
  ReadPolicy policy_;
  NodeBudget budget_;
  NodeBudget output_budget_;
  std::vector<NodePtr> stack_;
  std::unordered_map<std::uint64_t, NodePtr> memo_;
  std::size_t ops_ = 0;
};

Result<Json> parse_pickle(ByteView input, ReadContext& ctx) {
  PickleReader reader(input, ctx);
  CVT_TRY_ASSIGN(Json value, reader.parse());
  if (reader.converted_keys_ > 0) {
    ctx.warn(std::format("{} dictionary key(s) that were not strings were converted to text.", reader.converted_keys_));
  }
  return value;
}

}  // namespace

void register_pickle(Registry& registry) {
  registry.add_format(FormatInfo{
      .id = "pickle",
      .name = "Python pickle",
      .description = "Python's object serialisation; read as plain data only, never executed.",
      .category = Category::Data,
      .extensions = {"pkl", "pickle"},
      .mime = "application/x-python-pickle",
      .popularity = 20,
      .signatures = {Signature::at(0, "80 02"), Signature::at(0, "80 03"), Signature::at(0, "80 04 95"),
                     Signature::at(0, "80 05 95")},
      .sniff = [](ByteView, std::string_view filename) { return has_extension(filename, {"pkl", "pickle"}) ? 75 : 0; },
  });
  add_codec(registry, {.format_id = "pickle",
                       .read = binary_reader(parse_pickle),
                       .write = nullptr,
                       .read_options = common_read_options(true, false)});
}

}  // namespace cvt::data
