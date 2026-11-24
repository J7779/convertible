// Convertible core - pdf family: a parsed PDF file (xref, objects,
// decryption, page tree).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "families/pdf/filters.hpp"
#include "families/pdf/geom.hpp"
#include "families/pdf/object.hpp"
#include "util/bytes.hpp"

namespace cvt::pdf {

struct OpenOptions {
  std::string password;  ///< tried as user and as owner password; empty by default
  Limits limits;
  std::uint64_t max_objects = 4'000'000;
  std::uint64_t max_pages = 100'000;
  /// Accept the "%FDF-" header of form data files (same syntax).
  bool allow_fdf = false;
};

/// One page with inherited attributes resolved.
struct Page {
  Ref ref;
  Dict dict;            ///< the page dictionary itself
  Object resources;     ///< resolved Resources dict (inherited)
  Rect media_box{0, 0, 612, 792};
  Rect crop_box{0, 0, 612, 792};  ///< intersected with the media box
  int rotate = 0;       ///< 0, 90, 180 or 270
  double user_unit = 1.0;
};

class PdfFile {
 public:
  /// Parse the file structure. Fails only when nothing usable is found
  /// (not a PDF, or no catalog even after scanning) or the password is
  /// wrong; damaged parts are repaired and reported in `warnings()`.
  [[nodiscard]] static Result<std::shared_ptr<PdfFile>> open(ByteView data, const OpenOptions& options = {});

  /// The object `ref` (decrypted), null when absent or broken.
  [[nodiscard]] Object get(Ref ref);
  /// Follow references (bounded chain).
  [[nodiscard]] Object resolve(const Object& object);
  /// `dict[key]` resolved.
  [[nodiscard]] Object get(const Dict& dict, std::string_view key) { return resolve(dict.get(key)); }

  /// Decode a stream object (or reference to one) through its filters.
  /// Image codecs (DCT, JPX) are left pending unless `decode_images`.
  [[nodiscard]] Result<DecodedData> decode(const Object& stream, bool stop_at_images = true);
  /// Fully decoded bytes of a non-image stream; empty on any error.
  [[nodiscard]] Bytes decode_or_empty(const Object& stream);

  [[nodiscard]] const Dict& trailer() const noexcept { return trailer_; }
  [[nodiscard]] Object catalog() { return resolve(trailer_.get("Root")); }
  [[nodiscard]] Object info() { return resolve(trailer_.get("Info")); }
  [[nodiscard]] const std::vector<Page>& pages() const noexcept { return pages_; }
  [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
  [[nodiscard]] bool encrypted() const noexcept { return security_.has_value(); }
  [[nodiscard]] const std::string& version() const noexcept { return version_; }
  [[nodiscard]] const Limits& limits() const noexcept { return options_.limits; }
  [[nodiscard]] ByteView data() const noexcept { return data_; }
  /// All object numbers known to the cross-reference data (for rewriting).
  [[nodiscard]] std::vector<Ref> object_refs() const;
  /// Bytes decompressed so far by this file (all streams), for the limit.
  [[nodiscard]] std::uint64_t decompressed_total() const noexcept { return decompressed_total_; }
  void add_warning(std::string message);

 private:
  struct XrefEntry {
    enum class Kind : std::uint8_t { Free, Offset, Compressed };
    Kind kind = Kind::Free;
    std::uint64_t offset = 0;   ///< byte offset, or object-stream number
    std::uint32_t index = 0;    ///< index within the object stream
    std::uint32_t gen = 0;
  };
  struct Security;

  PdfFile(ByteView data, OpenOptions options);
  Result<void> load();
  bool load_xref_chain(std::size_t start);
  bool load_xref_table(std::size_t offset, Dict& trailer_out);
  bool load_xref_stream(std::size_t offset, Dict& trailer_out);
  void reconstruct();
  void ensure_reconstructed();
  Object parse_indirect_at(std::size_t offset, Ref expected, bool* ok);
  Object load_object(Ref ref);
  Object load_from_object_stream(std::uint32_t stream_num, std::uint32_t index, Ref ref);
  Result<void> setup_security(const std::string& password);
  void decrypt_object(Object& object, Ref ref, int depth);
  std::string decrypt_string(const std::string& bytes, Ref ref, bool is_stream, const std::string& crypt_filter);
  void load_pages();
  std::size_t find_stream_end(std::size_t data_start, std::size_t declared_length, bool length_known);

  ByteView data_;
  OpenOptions options_;
  std::size_t header_offset_ = 0;
  std::string version_ = "1.4";
  std::unordered_map<std::uint32_t, XrefEntry> xref_;
  std::unordered_map<std::uint32_t, XrefEntry> recovered_;
  bool reconstructed_ = false;
  Dict trailer_;
  std::unordered_map<std::uint32_t, Object> cache_;
  std::set<std::uint32_t> loading_;
  std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::size_t>>> objstm_index_;
  std::shared_ptr<Security> security_storage_;
  std::optional<int> security_;
  std::vector<Page> pages_;
  std::vector<std::string> warnings_;
  std::uint64_t decompressed_total_ = 0;
  Ref encrypt_ref_{};
};

/// Parse a date string "D:YYYYMMDDHHmmSSOHH'mm'" into ISO 8601 (best effort;
/// empty when unparseable).
[[nodiscard]] std::string pdf_date_to_iso(std::string_view text);

}  // namespace cvt::pdf
