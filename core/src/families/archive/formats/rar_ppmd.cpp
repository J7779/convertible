// Convertible core - archive family: PPMd model provider of the RAR 3 decoder.
// Adapts the PPMd variant H model with RAR's range coder (codecs/ppmd.hpp)
// to the interface the RAR 2.9-4.x decoder drives.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/archive/formats/rar_ppmd.hpp"

#include "families/archive/codecs/ppmd.hpp"

namespace cvt::archive::rar {

namespace {

namespace cz = cvt::compression::detail;

// Forwards byte requests to whichever RAR input the current call uses.
class Source final : public cz::PpmdByteSource {
 public:
  std::uint8_t next_byte() override { return input_ != nullptr ? input_->next() : 0; }
  void bind(PpmdInput& input) noexcept { input_ = &input; }

 private:
  PpmdInput* input_ = nullptr;
};

class Adapter final : public PpmdDecoder {
 public:
  bool restart(unsigned max_order, unsigned memory_mb) override {
    if (memory_mb == 0 || memory_mb > 256) return false;
    return model_.restart(static_cast<int>(max_order), static_cast<std::uint32_t>(memory_mb) << 20).has_value();
  }
  bool has_model() const override { return model_.has_model(); }
  void init_coder(PpmdInput& input) override {
    source_.bind(input);
    coder_ok_ = model_.init_range_coder(source_).has_value();
  }
  int decode(PpmdInput& input) override {
    if (!coder_ok_) return -1;
    source_.bind(input);
    const auto symbol = model_.decode_symbol();
    return symbol ? static_cast<int>(*symbol) : -1;
  }

 private:
  cz::RarPpmdDecoder model_;
  Source source_;
  bool coder_ok_ = false;
};

}  // namespace

std::unique_ptr<PpmdDecoder> make_ppmd_decoder() { return std::make_unique<Adapter>(); }

}  // namespace cvt::archive::rar
