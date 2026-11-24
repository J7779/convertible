// Convertible core - archive family: the PPMd interface of the RAR 3 decoder.
//
// RAR 2.9-4.x archives may switch, block by block, from LZ to PPMd variant H
// with RAR's own range coder. The decoder in rar_lz3.cpp parses the block
// header (flags, model size, escape character) itself and drives the model
// through this interface; `make_ppmd_decoder()` returns nullptr when the
// build has no PPMd model, and such blocks then fail with a precise message.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>

namespace cvt::archive::rar {

/// Bytes of compressed input, read one at a time by the range decoder.
class PpmdInput {
 public:
  virtual ~PpmdInput() = default;
  /// Next input byte; 0 once the input is exhausted.
  virtual std::uint8_t next() = 0;
};

class PpmdDecoder {
 public:
  virtual ~PpmdDecoder() = default;
  /// Start a fresh model of order `max_order` (2..64 after RAR's mapping)
  /// with `memory_mb` megabytes of model memory. False on failure.
  virtual bool restart(unsigned max_order, unsigned memory_mb) = 0;
  /// True when a model exists (a block without the reset flag continues it).
  [[nodiscard]] virtual bool has_model() const = 0;
  /// Initialise the range decoder (reads 4 bytes).
  virtual void init_coder(PpmdInput& input) = 0;
  /// Decode one byte; -1 on corrupt input.
  virtual int decode(PpmdInput& input) = 0;
};

/// The PPMd variant H model with RAR's range coder, or nullptr when this
/// build does not include it.
[[nodiscard]] std::unique_ptr<PpmdDecoder> make_ppmd_decoder();

}  // namespace cvt::archive::rar
