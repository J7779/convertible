// Convertible core - office family: Palm database containers and the
// compression schemes of Palm-era e-books (PalmDOC LZ77, Mobipocket
// HUFF/CDIC), shared by the MOBI/AZW3 and the plain Palm readers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "util/bytes.hpp"

namespace cvt::office::palm {

/// A Palm database (PDB/PRC): a header and a list of records.
struct Database {
  std::string name;     ///< database name (NUL-terminated in the file)
  std::string type;     ///< four-character type, e.g. "BOOK", "TEXt", "PNRd"
  std::string creator;  ///< four-character creator, e.g. "MOBI", "REAd", "PPrs"
  std::vector<ByteView> records;
};

/// Parse the header and record list; records are views into `data`.
[[nodiscard]] Result<Database> open(ByteView data, const Limits& limits);

/// PalmDOC (LZ77 variant) decompression; appends to `out`, never more than
/// `max_out` bytes in total.
[[nodiscard]] Result<void> palmdoc_decompress(ByteView in, Bytes& out, std::uint64_t max_out);
/// PalmDOC compression of one record (typically 4096 bytes).
[[nodiscard]] Bytes palmdoc_compress(ByteView in);

/// Mobipocket Huffman/CDIC dictionary decompressor.
class HuffCdic {
 public:
  /// `huff` is the HUFF record, `cdics` the CDIC records that follow it.
  [[nodiscard]] Result<void> load(ByteView huff, const std::vector<ByteView>& cdics);
  [[nodiscard]] Result<void> decompress(ByteView in, Bytes& out, std::uint64_t max_out);

 private:
  struct Code {
    std::uint32_t length = 0;
    bool terminal = false;
    std::uint64_t max = 0;
  };
  struct Phrase {
    ByteView data;
    bool literal = false;  ///< the bytes are final (no further Huffman coding)
    bool done = false;     ///< `cache` holds the expansion
    Bytes cache;
    bool busy = false;
  };
  [[nodiscard]] Result<void> unpack(ByteView in, Bytes& out, std::uint64_t max_out, int depth);
  Code dict1_[256]{};
  std::uint64_t min_code_[33]{};
  std::uint64_t max_code_[33]{};
  std::vector<Phrase> phrases_;
};

/// The size of the trailing entries of a MOBI text record (see the
/// "extra data flags" of the MOBI header).
[[nodiscard]] std::size_t trailing_size(ByteView record, std::uint32_t flags);

/// Write a Palm database.
[[nodiscard]] Bytes write_database(std::string_view name, std::string_view type, std::string_view creator,
                                   const std::vector<Bytes>& records);

}  // namespace cvt::office::palm
