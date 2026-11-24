// Convertible core - pdf family: serialising a new PDF file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/errors.hpp"
#include "families/pdf/object.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf {

/// Collects objects and writes a compact PDF: streams as indirect objects,
/// every other object packed into compressed object streams, and a
/// compressed cross-reference stream. Output is deterministic (the file ID
/// is a hash of the content).
class PdfBuilder {
 public:
  explicit PdfBuilder(int compression_level = 9) : level_(compression_level) {}

  /// A new object number (the object must be set before finish()).
  [[nodiscard]] Ref reserve();
  void set(Ref ref, Object object);
  [[nodiscard]] Ref add(Object object) {
    const Ref r = reserve();
    set(r, std::move(object));
    return r;
  }
  /// A stream whose data is compressed with Flate when that makes it
  /// smaller (`compress`); /Length and /Filter are filled in.
  void set_stream(Ref ref, Dict dict, ByteView data, bool compress = true);
  [[nodiscard]] Ref add_stream(Dict dict, ByteView data, bool compress = true) {
    const Ref r = reserve();
    set_stream(r, std::move(dict), data, compress);
    return r;
  }
  /// A stream whose data is already encoded as `dict` /Filter describes.
  void set_stream_raw(Ref ref, Dict dict, Bytes data);
  [[nodiscard]] Ref add_stream_raw(Dict dict, Bytes data) {
    const Ref r = reserve();
    set_stream_raw(r, std::move(dict), std::move(data));
    return r;
  }
  [[nodiscard]] std::size_t object_count() const noexcept { return next_ - 1; }

  /// Write the file. `info` may be absent.
  [[nodiscard]] Result<Bytes> finish(Ref root, std::optional<Ref> info, std::string_view version = "1.7",
                                     bool object_streams = true);

 private:
  struct Entry {
    bool set = false;
    bool stream = false;
    Object object;
    Bytes data;
  };
  int level_ = 9;
  std::uint32_t next_ = 1;
  std::map<std::uint32_t, Entry> objects_;
};

/// Flate-compress (zlib) at `level`.
[[nodiscard]] Bytes flate_encode(ByteView data, int level);

}  // namespace cvt::pdf
