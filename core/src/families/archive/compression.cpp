// Convertible core - archive family: the compression.hpp dispatcher.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/compression.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

#include "families/archive/codec_impl.hpp"
#include "util/text.hpp"

namespace cvt::compression {

namespace detail {

Result<void> check_output_room(std::uint64_t already, std::uint64_t extra, std::uint64_t max_output) {
  if (extra > max_output || already > max_output - extra) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", max_output);
  }
  return {};
}

}  // namespace detail

namespace {

using detail::CodecImpl;

struct NameEntry {
  Method method;
  std::string_view name;
};

constexpr std::array<NameEntry, 37> names = {{
    {Method::Store, "store"},
    {Method::Deflate, "deflate"},
    {Method::Zlib, "zlib"},
    {Method::Gzip, "gzip"},
    {Method::Bzip2, "bzip2"},
    {Method::Xz, "xz"},
    {Method::LzmaAlone, "lzma"},
    {Method::LzmaRaw, "lzma-raw"},
    {Method::Lzma2Raw, "lzma2-raw"},
    {Method::Lzip, "lzip"},
    {Method::Zstd, "zstd"},
    {Method::Lz4Frame, "lz4"},
    {Method::Lz4Block, "lz4-block"},
    {Method::Brotli, "brotli"},
    {Method::Snappy, "snappy"},
    {Method::SnappyFramed, "snappy-framed"},
    {Method::Lzw, "lzw"},
    {Method::Deflate64, "deflate64"},
    {Method::Mszip, "mszip"},
    {Method::Lzx, "lzx"},
    {Method::LzxWim, "lzx-wim"},
    {Method::Quantum, "quantum"},
    {Method::XpressPlain, "xpress"},
    {Method::XpressHuffman, "xpress-huffman"},
    {Method::Lzms, "lzms"},
    {Method::Lzo1x, "lzo1x"},
    {Method::Ppmd7, "ppmd7"},
    {Method::Ppmd8, "ppmd8"},
    {Method::Implode, "implode"},
    {Method::Shrink, "shrink"},
    {Method::Reduce, "reduce"},
    {Method::Lh, "lh"},
    {Method::Lzs, "lzs"},
    {Method::Arj, "arj"},
    {Method::Lzfse, "lzfse"},
    {Method::Adc, "adc"},
    {Method::Store, "copy"},
}};

constexpr std::array<NameEntry, 12> aliases = {{
    {Method::Gzip, "gz"},
    {Method::Bzip2, "bz2"},
    {Method::Zstd, "zst"},
    {Method::Zstd, "zstandard"},
    {Method::Brotli, "br"},
    {Method::Lzip, "lz"},
    {Method::Lzw, "z"},
    {Method::Lzw, "compress"},
    {Method::SnappyFramed, "sz"},
    {Method::Zlib, "zz"},
    {Method::Ppmd7, "ppmd"},
    {Method::Lzma2Raw, "lzma2"},
}};

using TableFn = std::span<const CodecImpl> (*)();
constexpr std::array<TableFn, 17> tables = {
    detail::deflate_codecs, detail::bzip2_codecs,    detail::lzma_codecs,  detail::zstd_codecs,
    detail::lz4_codecs,     detail::brotli_codecs,   detail::snappy_codecs, detail::lzw_codecs,
    detail::lzo_codecs,     detail::deflate64_codecs, detail::lzx_codecs,  detail::quantum_codecs,
    detail::xpress_codecs,  detail::lzh_codecs,      detail::zip_legacy_codecs, detail::ppmd_codecs,
    detail::apple_codecs,
};

const CodecImpl* find(Method method) noexcept {
  for (const TableFn table : tables) {
    for (const CodecImpl& impl : table()) {
      if (impl.method == method) return &impl;
    }
  }
  return nullptr;
}

std::unexpected<Error> unavailable(Method method, std::string_view direction) {
  return fail(Status::Decode, "{} {} is not available in this build", method_name(method), direction);
}

// Fallback streaming adapters for codecs without native incremental code:
// gather everything, then run the one-shot function.
class BufferingDecoder final : public Decoder {
 public:
  BufferingDecoder(const CodecImpl* impl, DecodeOptions options) : impl_(impl), options_(std::move(options)) {}

  Result<Step> decode(ByteView input, Bytes& output, bool last) override {
    if (error_) return fail(*error_);
    Step step;
    if (finished_) return step;
    append(input_, input);
    step.consumed = input.size();
    total_in_ += input.size();
    if (!last) return step;
    auto result = impl_->decode(input_, options_);
    input_.clear();
    input_.shrink_to_fit();
    if (!result) {
      error_ = result.error();
      return fail(*error_);
    }
    step.produced = result->size();
    total_out_ += result->size();
    append(output, *result);
    finished_ = true;
    step.finished = true;
    return step;
  }
  bool finished() const noexcept override { return finished_; }
  std::uint64_t total_in() const noexcept override { return total_in_; }
  std::uint64_t total_out() const noexcept override { return total_out_; }

 private:
  const CodecImpl* impl_;
  DecodeOptions options_;
  Bytes input_;
  std::optional<Error> error_;
  bool finished_ = false;
  std::uint64_t total_in_ = 0;
  std::uint64_t total_out_ = 0;
};

class BufferingEncoder final : public Encoder {
 public:
  BufferingEncoder(const CodecImpl* impl, EncodeOptions options) : impl_(impl), options_(std::move(options)) {}

  Result<void> encode(ByteView input, Bytes& /*output*/) override {
    if (done_) return fail(Status::InvalidArgument, "the encoder has already finished");
    append(input_, input);
    return {};
  }
  Result<void> finish(Bytes& output) override {
    if (done_) return fail(Status::InvalidArgument, "the encoder has already finished");
    done_ = true;
    CVT_TRY_ASSIGN(const Bytes packed, impl_->encode(input_, options_));
    total_in_ = input_.size();
    total_out_ = packed.size();
    input_.clear();
    input_.shrink_to_fit();
    append(output, packed);
    return {};
  }
  std::uint64_t total_in() const noexcept override { return done_ ? total_in_ : input_.size(); }
  std::uint64_t total_out() const noexcept override { return total_out_; }

 private:
  const CodecImpl* impl_;
  EncodeOptions options_;
  Bytes input_;
  bool done_ = false;
  std::uint64_t total_in_ = 0;
  std::uint64_t total_out_ = 0;
};

std::uint32_t le32(ByteView data, std::size_t at) noexcept {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(data[at + i]) << (8 * i);
  return v;
}

}  // namespace

std::string_view method_name(Method method) noexcept {
  for (const NameEntry& entry : names) {
    if (entry.method == method) return entry.name;
  }
  return "unknown";
}

std::optional<Method> parse_method(std::string_view name) noexcept {
  const std::string lowered = util::to_lower(util::trim(name));
  for (const NameEntry& entry : names) {
    if (entry.name == lowered) return entry.method;
  }
  for (const NameEntry& entry : aliases) {
    if (entry.name == lowered) return entry.method;
  }
  return std::nullopt;
}

bool can_decode(Method method) noexcept {
  const CodecImpl* impl = find(method);
  return impl != nullptr && impl->decode != nullptr;
}

bool can_encode(Method method) noexcept {
  const CodecImpl* impl = find(method);
  return impl != nullptr && impl->encode != nullptr;
}

Result<Bytes> decompress(Method method, ByteView data, const DecodeOptions& options) {
  const CodecImpl* impl = find(method);
  if (impl == nullptr || impl->decode == nullptr) return unavailable(method, "decoding");
  CVT_TRY_ASSIGN(Bytes out, impl->decode(data, options));
  if (options.stop_after != 0 && out.size() > options.stop_after) out.resize(static_cast<std::size_t>(options.stop_after));
  if (out.size() > options.max_output) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", options.max_output);
  }
  return out;
}

Result<Bytes> compress(Method method, ByteView data, const EncodeOptions& options) {
  const CodecImpl* impl = find(method);
  if (impl == nullptr || impl->encode == nullptr) return unavailable(method, "encoding");
  EncodeOptions clamped = options;
  clamped.level = std::clamp(options.level, 0, 9);
  return impl->encode(data, clamped);
}

Result<Bytes> encoder_properties(Method method, const EncodeOptions& options, std::uint64_t input_size) {
  const CodecImpl* impl = find(method);
  if (impl == nullptr || impl->encode == nullptr) return unavailable(method, "encoding");
  if (impl->properties == nullptr) return Bytes{};
  EncodeOptions clamped = options;
  clamped.level = std::clamp(options.level, 0, 9);
  return impl->properties(clamped, input_size);
}

Result<std::unique_ptr<Decoder>> make_decoder(Method method, const DecodeOptions& options) {
  const CodecImpl* impl = find(method);
  if (impl == nullptr || impl->decode == nullptr) return unavailable(method, "decoding");
  if (impl->make_decoder != nullptr) return impl->make_decoder(options);
  return std::unique_ptr<Decoder>(std::make_unique<BufferingDecoder>(impl, options));
}

Result<std::unique_ptr<Encoder>> make_encoder(Method method, const EncodeOptions& options) {
  const CodecImpl* impl = find(method);
  if (impl == nullptr || impl->encode == nullptr) return unavailable(method, "encoding");
  EncodeOptions clamped = options;
  clamped.level = std::clamp(options.level, 0, 9);
  if (impl->make_encoder != nullptr) return impl->make_encoder(clamped);
  return std::unique_ptr<Encoder>(std::make_unique<BufferingEncoder>(impl, std::move(clamped)));
}

Result<std::size_t> filter_decode(const Filter& filter, std::span<std::byte> data, FilterState& state, bool last) {
  return detail::filter_decode_impl(filter, data, state, last);
}

Result<std::size_t> filter_encode(const Filter& filter, std::span<std::byte> data, FilterState& state, bool last) {
  return detail::filter_encode_impl(filter, data, state, last);
}

Result<Bytes> bcj2_decode(ByteView main_stream, ByteView call_stream, ByteView jump_stream, ByteView range_stream,
                          std::uint64_t output_size, std::uint64_t max_output) {
  return detail::bcj2_decode_impl(main_stream, call_stream, jump_stream, range_stream, output_size, max_output);
}

std::optional<Method> detect(ByteView data) noexcept {
  const auto at = [&](std::size_t i) { return i < data.size() ? std::to_integer<std::uint8_t>(data[i]) : 0u; };
  const auto starts = [&](std::string_view magic) {
    return data.size() >= magic.size() && as_chars(data.first(magic.size())) == magic;
  };
  if (at(0) == 0x1F && at(1) == 0x8B && data.size() >= 3) return Method::Gzip;
  if (starts("BZh") && at(3) >= '1' && at(3) <= '9') return Method::Bzip2;
  if (starts(std::string_view("\xFD" "7zXZ\0", 6))) return Method::Xz;
  if (starts("LZIP")) return Method::Lzip;
  if (data.size() >= 4) {
    const std::uint32_t magic = le32(data, 0);
    if (magic == 0xFD2FB528u) return Method::Zstd;
    if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) return Method::Zstd;  // skippable frame (also LZ4 skippable)
    if (magic == 0x184D2204u) return Method::Lz4Frame;
  }
  if (starts(std::string_view("\xFF\x06\x00\x00sNaPpY", 10))) return Method::SnappyFramed;
  if (at(0) == 0x1F && at(1) == 0x9D && data.size() >= 3) return Method::Lzw;
  // .lzma: properties byte (almost always 0x5D = lc3 lp0 pb2), a dictionary
  // of 2^n or 3*2^n bytes, an uncompressed size that is unknown (all FF) or
  // below 2^48, and the range coder's mandatory leading zero byte.
  if (data.size() >= 14 && at(0) == 0x5D && at(13) == 0x00) {
    const std::uint32_t dict = le32(data, 1);
    bool all_ff = true;
    for (std::size_t i = 5; i < 13; ++i) all_ff = all_ff && at(i) == 0xFF;
    const bool size_ok = all_ff || (at(12) == 0 && at(11) == 0);
    const bool dict_ok = dict >= (1u << 12) && (std::has_single_bit(dict) || (dict % 3 == 0 && std::has_single_bit(dict / 3)));
    if (size_ok && dict_ok) return Method::LzmaAlone;
  }
  // zlib: CMF/FLG check bits, method 8, window <= 32 KiB, no preset dictionary.
  if (data.size() >= 2 && (at(0) & 0x0Fu) == 8 && (at(0) >> 4) <= 7 && (at(1) & 0x20u) == 0 &&
      ((at(0) << 8) | at(1)) % 31 == 0) {
    return Method::Zlib;
  }
  return std::nullopt;
}

}  // namespace cvt::compression
