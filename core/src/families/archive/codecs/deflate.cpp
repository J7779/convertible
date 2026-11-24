// Convertible core - archive family: Store, Deflate, zlib and gzip behind compression.hpp.
// The DEFLATE engine itself is the shared util/deflate implementation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <memory>
#include <optional>

#include "families/archive/codec_impl.hpp"
#include "util/deflate.hpp"

namespace cvt::compression::detail {

namespace {

util::DeflateFormat format_of(Method method) {
  switch (method) {
    case Method::Zlib: return util::DeflateFormat::Zlib;
    case Method::Gzip: return util::DeflateFormat::Gzip;
    default: return util::DeflateFormat::Raw;
  }
}

std::uint64_t effective_limit(const DecodeOptions& options) {
  std::uint64_t limit = options.max_output;
  if (options.output_size) limit = std::min(limit, *options.output_size);
  if (options.stop_after != 0) limit = std::min(limit, options.stop_after);
  return limit;
}

// ---- incremental -----------------------------------------------------------

class InflateDecoder final : public Decoder {
 public:
  InflateDecoder(Method method, const DecodeOptions& options)
      : method_(method), inflater_(format_of(method)), options_(options) {}

  Result<Step> decode(ByteView input, Bytes& output, bool last) override {
    if (error_) return fail(*error_);
    Step step;
    while (!finished_) {
      if (between_members_) {
        // A gzip file may hold several members back to back.
        if (input.empty()) {
          if (last) finished_ = true;
          break;
        }
        if (std::to_integer<int>(input[0]) != 0x1F) {
          finished_ = true;  // trailing data after the last member
          break;
        }
        previous_out_ += inflater_.total_out();
        inflater_.reset();
        between_members_ = false;
      }
      // A prefix decode runs a little past the requested size (a match may
      // cross it) and is cut back exactly below.
      const std::uint64_t ceiling = options_.stop_after != 0
                                        ? std::min(options_.stop_after + prefix_slack, options_.max_output)
                                        : options_.max_output;
      const std::uint64_t room = ceiling == unlimited ? unlimited : ceiling - std::min(ceiling, previous_out_);
      const std::size_t before = output.size();
      auto progress = inflater_.feed(input, output, room);
      step.produced += output.size() - before;
      if (options_.stop_after != 0 && previous_out_ + inflater_.total_out() >= options_.stop_after) {
        const std::uint64_t excess = previous_out_ + inflater_.total_out() - options_.stop_after;
        const std::size_t cut = static_cast<std::size_t>(std::min<std::uint64_t>(excess, output.size() - before));
        output.resize(output.size() - cut);
        step.produced -= cut;
        prefix_done_ = true;
        finished_ = true;
        step.consumed += progress ? progress->consumed : input.size();
        total_in_ += progress ? progress->consumed : input.size();
        break;
      }
      if (!progress) {
        if (options_.stop_after != 0 && progress.error().code == Status::Limit) {
          finished_ = true;  // prefix decode: enough output
          step.consumed += input.size();
          total_in_ += input.size();
          break;
        }
        error_ = progress.error();
        return fail(*error_);
      }
      step.consumed += progress->consumed;
      total_in_ += progress->consumed;
      input = input.subspan(progress->consumed);
      if (!progress->finished) {
        if (last) {
          error_ = Error{Status::Decode, std::string(method_name(method_)) + " stream is truncated"};
          return fail(*error_);
        }
        break;
      }
      if (method_ != Method::Gzip) {
        finished_ = true;
        break;
      }
      between_members_ = true;
    }
    step.finished = finished_;
    return step;
  }
  bool finished() const noexcept override { return finished_; }
  std::uint64_t total_in() const noexcept override { return total_in_; }
  std::uint64_t total_out() const noexcept override {
    const std::uint64_t all = previous_out_ + inflater_.total_out();
    return prefix_done_ ? std::min(all, options_.stop_after) : all;
  }

 private:
  Method method_;
  util::Inflater inflater_;
  DecodeOptions options_;
  std::optional<Error> error_;
  bool finished_ = false;
  bool between_members_ = false;
  bool prefix_done_ = false;
  static constexpr std::uint64_t prefix_slack = 1024;
  std::uint64_t total_in_ = 0;
  std::uint64_t previous_out_ = 0;
};

class DeflateEncoder final : public Encoder {
 public:
  DeflateEncoder(Method method, const EncodeOptions& options)
      : deflater_(format_of(method), options.level,
                  util::GzipHeader{options.original_name, options.mtime}) {}
  Result<void> encode(ByteView input, Bytes& output) override { return deflater_.feed(input, output); }
  Result<void> finish(Bytes& output) override { return deflater_.finish(output); }
  std::uint64_t total_in() const noexcept override { return deflater_.total_in(); }
  std::uint64_t total_out() const noexcept override { return deflater_.total_out(); }

 private:
  util::Deflater deflater_;
};

// ---- one-shot ----------------------------------------------------------------

Result<Bytes> decode_store(ByteView data, const DecodeOptions& options) {
  const std::uint64_t limit = effective_limit(options);
  if (data.size() > limit && options.stop_after == 0 && !options.output_size) {
    return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", options.max_output);
  }
  if (options.output_size && data.size() < *options.output_size) {
    return fail(Status::Decode, "stored data is truncated");
  }
  return Bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(data.size(), limit)));
}

Result<Bytes> encode_store(ByteView data, const EncodeOptions&) { return Bytes(data.begin(), data.end()); }

Result<Bytes> decode_inflate(Method method, ByteView data, const DecodeOptions& options) {
  InflateDecoder decoder(method, options);
  Bytes out;
  CVT_TRY_ASSIGN(const Step step, decoder.decode(data, out, true));
  (void)step;
  if (options.output_size && options.stop_after == 0 && out.size() != *options.output_size) {
    return fail(Status::Decode, "{} data decodes to {} bytes, expected {}", method_name(method), out.size(),
                *options.output_size);
  }
  return out;
}

Result<Bytes> encode_deflate(Method method, ByteView data, const EncodeOptions& options) {
  DeflateEncoder encoder(method, options);
  Bytes out;
  // Fed in pieces (the output does not depend on how the input is cut) so
  // that progress is reported and a cancellation is seen every 256 KiB.
  constexpr std::size_t piece = std::size_t{256} << 10;
  for (std::size_t at = 0; at < data.size(); at += piece) {
    if (!options.progress.report(static_cast<double>(at) / static_cast<double>(data.size()))) {
      return fail(Status::Cancelled, "the conversion was cancelled");
    }
    CVT_TRY(encoder.encode(data.subspan(at, std::min(piece, data.size() - at)), out));
  }
  CVT_TRY(encoder.finish(out));
  return out;
}

template <Method M>
Result<Bytes> decode_one(ByteView data, const DecodeOptions& options) {
  return decode_inflate(M, data, options);
}
template <Method M>
Result<Bytes> encode_one(ByteView data, const EncodeOptions& options) {
  return encode_deflate(M, data, options);
}
template <Method M>
std::unique_ptr<Decoder> make_dec(const DecodeOptions& options) {
  return std::make_unique<InflateDecoder>(M, options);
}
template <Method M>
std::unique_ptr<Encoder> make_enc(const EncodeOptions& options) {
  return std::make_unique<DeflateEncoder>(M, options);
}

constexpr std::array<CodecImpl, 4> table = {{
    {Method::Store, decode_store, encode_store, nullptr, nullptr, nullptr},
    {Method::Deflate, decode_one<Method::Deflate>, encode_one<Method::Deflate>, make_dec<Method::Deflate>,
     make_enc<Method::Deflate>, nullptr},
    {Method::Zlib, decode_one<Method::Zlib>, encode_one<Method::Zlib>, make_dec<Method::Zlib>, make_enc<Method::Zlib>,
     nullptr},
    {Method::Gzip, decode_one<Method::Gzip>, encode_one<Method::Gzip>, make_dec<Method::Gzip>, make_enc<Method::Gzip>,
     nullptr},
}};

}  // namespace

std::span<const CodecImpl> deflate_codecs() { return table; }

}  // namespace cvt::compression::detail
