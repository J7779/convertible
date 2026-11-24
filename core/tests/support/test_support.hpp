// Shared helpers for the core test suite.
//
// Family tests should go through these rather than poking at engine
// internals: they exercise codecs exactly the way the engine does (options
// validated against the codec's specs, limits and warnings wired up).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "support/test.hpp"

#include "engine/convert.hpp"
#include "engine/registry.hpp"
#include "util/bytes.hpp"

namespace cvt {

/// Lets failure messages print statuses by name.
inline std::ostream& operator<<(std::ostream& out, Status status) { return out << status_name(status); }

}  // namespace cvt

namespace cvt::test {

/// Text literal -> bytes, and back.
[[nodiscard]] inline Bytes bytes(std::string_view text) { return to_bytes(text); }
[[nodiscard]] inline std::string str(ByteView data) { return to_string(data); }
[[nodiscard]] inline std::string str(const Bytes& data) { return to_string(ByteView(data)); }

/// `count` deterministic pseudo-random bytes (same output on every platform).
[[nodiscard]] inline Bytes random_bytes(std::size_t count, std::uint32_t seed = 1) {
  Bytes out(count);
  std::uint32_t state = seed * 2654435761u + 1u;
  for (std::byte& b : out) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    b = static_cast<std::byte>(state >> 24);
  }
  return out;
}

/// Every byte value 0..255 once, in order.
[[nodiscard]] inline Bytes all_byte_values() {
  Bytes out(256);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::byte>(i);
  return out;
}

/// Outcome of a conversion run through the shared registry.
struct Converted {
  Status status = Status::Ok;
  std::string message;
  Bytes data;
  std::string format_id;
  std::string extension;
  std::string mime;
  std::vector<std::string> warnings;

  [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }
  [[nodiscard]] std::string text() const { return to_string(ByteView(data)); }
};

/// Convert `input` from `source` (empty = detect) to `target` with the real
/// registry, exactly as `cvt_convert` would.
[[nodiscard]] inline Converted convert(ByteView input, std::string_view source, std::string_view target,
                                       std::string_view options_json = {}, std::string_view filename = {},
                                       const Limits& limits = {}) {
  Converted out;
  auto options = Options::parse(options_json);
  if (!options) {
    out.status = options.error().code;
    out.message = options.error().message;
    return out;
  }
  ConvertRequest request;
  request.input = input;
  request.filename = std::string(filename);
  request.source_id = std::string(source);
  request.target_id = std::string(target);
  request.options = std::move(*options);
  request.limits = limits;
  auto result = run_conversion(Registry::instance(), request);
  if (!result) {
    out.status = result.error().code;
    out.message = result.error().message;
    return out;
  }
  out.data = std::move(result->data);
  out.format_id = std::move(result->format_id);
  out.extension = std::move(result->extension);
  out.mime = std::move(result->mime);
  out.warnings = std::move(result->warnings);
  return out;
}
[[nodiscard]] inline Converted convert(std::string_view input, std::string_view source, std::string_view target,
                                       std::string_view options_json = {}, std::string_view filename = {},
                                       const Limits& limits = {}) {
  return convert(as_bytes(input), source, target, options_json, filename, limits);
}

/// The registered codec for (format, kind) that can read / write, or nullptr.
[[nodiscard]] inline const Codec* find_reader(std::string_view format_id, Kind kind) {
  for (const Codec* codec : Registry::instance().codecs_for(format_id)) {
    if (codec->kind == kind && codec->read) return codec;
  }
  return nullptr;
}
[[nodiscard]] inline const Codec* find_writer(std::string_view format_id, Kind kind) {
  for (const Codec* codec : Registry::instance().codecs_for(format_id)) {
    if (codec->kind == kind && codec->write) return codec;
  }
  return nullptr;
}

/// Decode `input` with the registered reader of `format_id` for IR type T:
///   auto image = cvt::test::read_as<ir::Image>("png", png_bytes);
///   REQUIRE(image);
/// `warnings`, when given, receives the notes the codec emitted.
template <IrType T>
[[nodiscard]] Result<T> read_as(std::string_view format_id, ByteView input, std::string_view options_json = {},
                                std::vector<std::string>* warnings = nullptr, const Limits& limits = {}) {
  const Codec* codec = find_reader(format_id, kind_of<T>);
  if (codec == nullptr) {
    return fail(Status::UnsupportedConversion, "no {} reader registered for '{}'", kind_name(kind_of<T>), format_id);
  }
  CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
  ReadContext ctx;
  CVT_TRY_ASSIGN(ctx.options, options.normalized(codec->read_options));
  ctx.limits = limits;
  ctx.registry = &Registry::instance();
  ctx.input = input;
  ctx.format_id = std::string(format_id);
  auto value = codec->read(ctx);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  if (!value) return std::unexpected(std::move(value).error());
  if (!value->template is<T>()) return fail(Status::Internal, "reader returned {}", kind_name(value->kind()));
  return std::move(*value).template get<T>();
}

/// Encode an IR value with the registered writer of `format_id`:
///   auto png = cvt::test::write_from("png", image, R"({"compression":9})");
/// Copyable IRs can be passed as lvalues; move-only ones (ir::Video) must be
/// moved in.
[[nodiscard]] inline Result<Bytes> write_from(std::string_view format_id, AnyIR value,
                                              std::string_view options_json = {},
                                              std::vector<std::string>* warnings = nullptr, const Limits& limits = {}) {
  const Codec* codec = find_writer(format_id, value.kind());
  if (codec == nullptr) {
    return fail(Status::UnsupportedConversion, "no {} writer registered for '{}'", kind_name(value.kind()), format_id);
  }
  CVT_TRY_ASSIGN(const Options options, Options::parse(options_json));
  WriteContext ctx;
  CVT_TRY_ASSIGN(ctx.options, options.normalized(codec->write_options));
  ctx.limits = limits;
  ctx.registry = &Registry::instance();
  ctx.format_id = std::string(format_id);
  auto encoded = codec->write(value, ctx);
  if (warnings != nullptr) *warnings = ctx.warnings.list();
  return encoded;
}

/// Feed a reader every prefix of `valid` (to catch out-of-bounds reads on
/// truncated files) and a set of deterministic corruptions. The codec may
/// succeed or fail, but must return; run the suite under the `asan` preset
/// to turn memory errors into failures. Returns the number of inputs tried.
template <IrType T>
std::size_t fuzz_reader(std::string_view format_id, ByteView valid, std::size_t max_prefixes = 512) {
  std::size_t tried = 0;
  const std::size_t step = valid.size() / max_prefixes + 1;
  for (std::size_t length = 0; length < valid.size(); length += step) {
    (void)read_as<T>(format_id, valid.first(length));
    ++tried;
  }
  for (std::uint32_t seed = 1; seed <= 64 && !valid.empty(); ++seed) {
    Bytes mutated(valid.begin(), valid.end());
    const Bytes noise = random_bytes(8, seed);
    for (std::size_t k = 0; k < 4; ++k) {
      const std::size_t position =
          (std::to_integer<std::size_t>(noise[k * 2]) * 131u + seed * 7919u + k * 104729u) % mutated.size();
      mutated[position] = noise[k * 2 + 1];
    }
    (void)read_as<T>(format_id, mutated);
    ++tried;
  }
  (void)read_as<T>(format_id, random_bytes(1024, 99));
  return tried + 1;
}

}  // namespace cvt::test
