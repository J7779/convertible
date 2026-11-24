// Convertible core - archive family: LZMA model, range coder, decoder and encoder (shared by
// LZMA alone, raw LZMA, LZMA2, xz, lzip and 7z).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/errors.hpp"
#include "engine/progress.hpp"
#include "util/bytes.hpp"

namespace cvt::compression::detail::lzma {

inline constexpr int num_states = 12;
inline constexpr int pos_bits_max = 4;
inline constexpr int len_to_pos_states = 4;
inline constexpr int align_bits = 4;
inline constexpr int start_pos_model = 4;
inline constexpr int end_pos_model = 14;
inline constexpr int num_full_distances = 128;
inline constexpr int match_min_len = 2;
inline constexpr int match_max_len = 273;
inline constexpr std::uint32_t prob_init = 1024;

struct Properties {
  int lc = 3;
  int lp = 0;
  int pb = 2;
  std::uint32_t dict_size = 1u << 23;
  [[nodiscard]] std::uint8_t byte() const noexcept { return static_cast<std::uint8_t>((pb * 5 + lp) * 9 + lc); }
  [[nodiscard]] static bool from_byte(std::uint8_t b, Properties& out) noexcept {
    if (b >= 9 * 5 * 5) return false;
    out.lc = b % 9;
    out.lp = (b / 9) % 5;
    out.pb = b / 45;
    return true;
  }
};

/// Probability model of one LZMA coder.
struct Model {
  std::vector<std::uint16_t> literal;  // 0x300 << (lc + lp)
  std::array<std::uint16_t, num_states << pos_bits_max> is_match{};
  std::array<std::uint16_t, num_states> is_rep{};
  std::array<std::uint16_t, num_states> is_rep_g0{};
  std::array<std::uint16_t, num_states> is_rep_g1{};
  std::array<std::uint16_t, num_states> is_rep_g2{};
  std::array<std::uint16_t, num_states << pos_bits_max> is_rep0_long{};
  std::array<std::uint16_t, len_to_pos_states << 6> pos_slot{};
  std::array<std::uint16_t, 1 + num_full_distances - end_pos_model> spec_pos{};
  std::array<std::uint16_t, 1 << align_bits> align{};
  struct Length {
    std::uint16_t choice = prob_init;
    std::uint16_t choice2 = prob_init;
    std::array<std::uint16_t, 16 << 3> low{};
    std::array<std::uint16_t, 16 << 3> mid{};
    std::array<std::uint16_t, 256> high{};
  };
  Length len;
  Length rep_len;

  void reset(int lc, int lp);
};

/// State shared by decoder and encoder between chunks.
struct CoderState {
  int state = 0;
  std::array<std::uint32_t, 4> reps{};
  void reset() noexcept {
    state = 0;
    reps = {0, 0, 0, 0};
  }
};

[[nodiscard]] constexpr int state_after_literal(int s) noexcept { return s < 4 ? 0 : (s < 10 ? s - 3 : s - 6); }
[[nodiscard]] constexpr int state_after_match(int s) noexcept { return s < 7 ? 7 : 10; }
[[nodiscard]] constexpr int state_after_rep(int s) noexcept { return s < 7 ? 8 : 11; }
[[nodiscard]] constexpr int state_after_short_rep(int s) noexcept { return s < 7 ? 9 : 11; }

// ------------------------------------------------------------------------------- decoder

/// Decodes LZMA symbols into `out`, which doubles as the dictionary: matches
/// may reach back to `dict_start` (an LZMA2 dictionary reset moves it).
class Decoder {
 public:
  void set_properties(const Properties& props);
  void reset_state();
  /// Forget the dictionary (LZMA2 dictionary reset).
  void reset_dictionary(std::size_t out_size) noexcept { dict_start_ = out_size; }

  struct Outcome {
    std::size_t consumed = 0;   ///< input bytes used by the range decoder
    bool end_marker = false;    ///< the stream ended with an end marker
  };
  /// Decode one range-coded run from `input` until `out` holds `out_limit`
  /// bytes (exactly; `stop_at_limit`) or an end marker appears (when
  /// `allow_end_marker`). `max_output` bounds out.size() (Status::Limit).
  [[nodiscard]] Result<Outcome> decode(ByteView input, Bytes& out, std::uint64_t out_limit, bool allow_end_marker,
                                       std::uint64_t max_output, const Progress& progress);

 private:
  Properties props_{};
  Model model_;
  CoderState st_;
  std::size_t dict_start_ = 0;
  bool have_props_ = false;
};

// ------------------------------------------------------------------------------- encoder

struct EncoderParams {
  Properties props;
  int nice_len = 64;
  int depth = 48;       ///< match-finder chain depth
  bool optimal = true;  ///< price-based parsing (false = fast greedy/lazy)
};

/// Parameters for an xz-style preset level 0..9; `input_size` caps the dictionary.
[[nodiscard]] EncoderParams preset(int level, std::uint64_t input_size, std::uint32_t dict_override);

/// LZMA encoder over a buffer that is entirely in memory. Positions refer to
/// that buffer; earlier positions form the dictionary.
class Encoder {
 public:
  Encoder(ByteView data, const EncoderParams& params);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  /// Reset probabilities, state and reps (new stream or LZMA2 state reset).
  void reset_state();
  /// Encode from the current position. Stops when the input ends, when
  /// `max_in` bytes were consumed, or when about `max_out` compressed bytes
  /// were produced (LZMA2 chunk limits). Appends the flushed range coder
  /// output (and an end marker when `end_marker` and the input ended).
  /// Returns the number of input bytes consumed.
  [[nodiscard]] Result<std::size_t> encode(Bytes& out, std::size_t max_in, std::size_t max_out, bool end_marker,
                                           const Progress& progress);
  /// Bytes of the buffer consumed so far.
  [[nodiscard]] std::size_t position() const noexcept;
  [[nodiscard]] bool at_end() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvt::compression::detail::lzma
