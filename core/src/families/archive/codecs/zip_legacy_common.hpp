// Convertible core - archive family: output buffer and helpers shared by the decode-only legacy codecs
// (Deflate64, MSZIP, LZX, Quantum, XPRESS, LHA/ARJ, PKZIP shrink/reduce/implode, LZO1X).
//
// The decoders write into `LegacyOutput`, which enforces the DecodeOptions
// contract in one place:
//   * `output_size` given: the decoder produces exactly that many bytes (a
//     stream that ends sooner is truncated, Status::Decode); a declared size
//     above `max_output` is Status::Limit before anything is allocated.
//   * no `output_size`: the decoder runs until its data ends; producing more
//     than `max_output` is Status::Limit.
//   * `stop_after`: decoding stops successfully after that many bytes.
// Memory grows with the data actually produced, never with a declared size.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "util/bytes.hpp"
#include "families/archive/codec_impl.hpp"

namespace cvt::compression::detail {

class LegacyOutput {
 public:
  /// Validate the options; `name` is used in error messages.
  [[nodiscard]] Result<void> start(const DecodeOptions& options, std::string_view name) {
    name_ = name;
    max_output_ = options.max_output;
    if (!options.progress.report(0.0)) return fail(Status::Cancelled, "the conversion was cancelled");
    if (options.output_size) {
      if (*options.output_size > options.max_output) {
        return fail(Status::Limit, "{}: declared size {} exceeds the limit of {} bytes", name, *options.output_size,
                    options.max_output);
      }
      exact_ = true;
      target_ = *options.output_size;
    } else {
      target_ = options.max_output;
    }
    if (options.stop_after != 0 && options.stop_after < target_) {
      target_ = options.stop_after;
      prefix_ = true;
    }
    if (target_ > static_cast<std::uint64_t>(SIZE_MAX) - 16) target_ = static_cast<std::uint64_t>(SIZE_MAX) - 16;
    return {};
  }

  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] bool done() const noexcept { return n_ >= target_; }
  [[nodiscard]] std::uint64_t room() const noexcept { return target_ - n_; }
  [[nodiscard]] std::uint64_t target() const noexcept { return target_; }
  [[nodiscard]] bool exact() const noexcept { return exact_; }
  [[nodiscard]] bool prefix() const noexcept { return prefix_; }
  [[nodiscard]] std::uint8_t* data() noexcept { return reinterpret_cast<std::uint8_t*>(buf_.data()); }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return reinterpret_cast<const std::uint8_t*>(buf_.data()); }
  [[nodiscard]] std::uint8_t at(std::size_t i) const noexcept { return data()[i]; }

  /// Make room for `extra` more bytes (the caller has clipped `extra` to `room()`).
  void reserve_more(std::size_t extra) {
    if (n_ + extra <= buf_.size()) return;
    std::uint64_t want = std::max<std::uint64_t>(std::uint64_t{buf_.size()} * 2, 1u << 16);
    want = std::max<std::uint64_t>(want, std::uint64_t{n_} + extra);
    want = std::min<std::uint64_t>(want, target_ + 16);
    buf_.resize(static_cast<std::size_t>(want));
  }

  /// Append one byte; the caller has checked `!done()`.
  void put(std::uint8_t byte) {
    if (n_ == buf_.size()) reserve_more(1);
    data()[n_++] = byte;
  }

  /// Append `length` bytes copied from `distance` bytes back (overlap allowed),
  /// clipped to the remaining room. False when `distance` reaches before the
  /// start of the output.
  [[nodiscard]] bool copy(std::size_t distance, std::size_t length) {
    if (distance == 0 || distance > n_) return false;
    if (length > room()) {
      overflow_ = true;
      length = static_cast<std::size_t>(room());
    }
    reserve_more(length);
    std::uint8_t* d = data() + n_;
    const std::uint8_t* s = d - distance;
    if (distance >= length) {
      ::cvt::util::copy_bytes(d, s, length);
    } else {
      for (std::size_t i = 0; i < length; ++i) d[i] = s[i];
    }
    n_ += length;
    return true;
  }

  /// Record that the stream wanted more output than the room left.
  void mark_overflow() noexcept { overflow_ = true; }

  /// Append raw bytes, clipped to the remaining room.
  void append_raw(const std::uint8_t* src, std::size_t length) {
    if (length > room()) {
      overflow_ = true;
      length = static_cast<std::size_t>(room());
    }
    reserve_more(length);
    if (length != 0) ::cvt::util::copy_bytes(data() + n_, src, length);
    n_ += length;
  }

  /// Finish: `more_input` = the stream still had data when the output was
  /// full; that, or a copy cut at the end, is Status::Limit when no exact
  /// size was declared and this is not a prefix request. An exact size that was not reached is a truncated stream.
  [[nodiscard]] Result<Bytes> finish(bool more_input) {
    if (exact_ && !prefix_ && n_ < target_) return fail(Status::Decode, "{} data is truncated", name_);
    if (!exact_ && !prefix_ && (more_input || overflow_) && n_ >= target_) {
      return fail(Status::Limit, "decompressed data exceeds the limit of {} bytes", max_output_);
    }
    buf_.resize(n_);
    return std::move(buf_);
  }

 private:
  Bytes buf_;
  std::size_t n_ = 0;
  std::uint64_t target_ = 0;
  std::uint64_t max_output_ = 0;
  std::string_view name_;
  bool exact_ = false;
  bool prefix_ = false;
  bool overflow_ = false;  // a copy was cut at the end of the output
};

/// Progress helper: report about every `step` produced bytes.
[[nodiscard]] inline bool legacy_progress(const DecodeOptions& options, std::uint64_t consumed, std::uint64_t total) {
  if (total == 0) return true;
  return options.progress.report(static_cast<double>(consumed) / static_cast<double>(total));
}

[[nodiscard]] inline std::uint16_t legacy_le16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
[[nodiscard]] inline std::uint32_t legacy_le32(const std::uint8_t* p) noexcept {
  return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

}  // namespace cvt::compression::detail
