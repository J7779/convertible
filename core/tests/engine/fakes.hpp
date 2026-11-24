// Fake formats, codecs and bridges for exercising the engine in isolation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "engine/registry.hpp"
#include "support/test_support.hpp"

namespace cvt::test {

inline FormatInfo fake_format(std::string id, Category category = Category::Other, int popularity = 50,
                              std::vector<std::string> extensions = {}) {
  FormatInfo info;
  info.id = id;
  info.name = "Fake " + id;
  info.category = category;
  info.popularity = popularity;
  info.extensions = extensions.empty() ? std::vector<std::string>{id} : std::move(extensions);
  return info;
}

/// A reader that wraps the input bytes in the requested (default-constructed) IR kind.
inline std::function<Result<AnyIR>(ReadContext&)> fake_read(Kind kind) {
  return [kind](ReadContext& ctx) -> Result<AnyIR> {
    switch (kind) {
      case Kind::Blob: {
        ir::Blob blob;
        blob.bytes.assign(ctx.input.begin(), ctx.input.end());
        return blob;
      }
      case Kind::PlainText: return ir::PlainText{to_string(ctx.input), "utf-8", false};
      case Kind::Image: return ir::Image{};
      case Kind::Document: return ir::Document{};
      case Kind::Table: return ir::Table{};
      case Kind::Data: return ir::Data{};
      case Kind::Audio: return ir::Audio{};
      default: return ir::Archive{};
    }
  };
}

/// A writer that emits "<format>:<kind>" plus the text of PlainText/Blob values.
inline std::function<Result<Bytes>(const AnyIR&, WriteContext&)> fake_write() {
  return [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
    std::string out = ctx.format_id + ":" + std::string(kind_name(value.kind()));
    if (const auto* text = value.get_if<ir::PlainText>()) out += ":" + text->text;
    if (const auto* blob = value.get_if<ir::Blob>()) out += ":" + to_string(blob->bytes);
    return to_bytes(out);
  };
}

inline Codec fake_codec(std::string format_id, Kind kind, bool can_read, bool can_write, int read_cost = 10,
                        int write_cost = 10, bool lossy = false) {
  Codec codec;
  codec.format_id = std::move(format_id);
  codec.kind = kind;
  if (can_read) codec.read = fake_read(kind);
  if (can_write) codec.write = fake_write();
  codec.read_cost = read_cost;
  codec.write_cost = write_cost;
  codec.lossy = lossy;
  return codec;
}

/// A bridge that discards its input and yields a default value of kind `to`.
inline Bridge fake_bridge(Kind from, Kind to, int cost = 10, bool lossy = false, std::string label = {}) {
  Bridge bridge;
  bridge.from = from;
  bridge.to = to;
  bridge.cost = cost;
  bridge.lossy = lossy;
  bridge.label = label.empty() ? std::string(kind_name(from)) + "->" + std::string(kind_name(to)) : std::move(label);
  bridge.fn = [to](AnyIR value, Context&) -> Result<AnyIR> {
    switch (to) {
      case Kind::Blob: return ir::Blob{};
      case Kind::PlainText: {
        ir::PlainText text;
        if (const auto* in = value.get_if<ir::PlainText>()) text.text = in->text;
        return text;
      }
      case Kind::Image: return ir::Image{};
      case Kind::Document: return ir::Document{};
      case Kind::Table: return ir::Table{};
      case Kind::Data: return ir::Data{};
      case Kind::Audio: return ir::Audio{};
      default: return ir::Archive{};
    }
  };
  return bridge;
}

}  // namespace cvt::test
