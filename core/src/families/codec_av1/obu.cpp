// Convertible core - codec_av1 family: AV1 bitstream syntax (OBUs, sequence
// header, av1C, Annex B).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_av1/obu.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace cvt::av1 {

namespace {

constexpr std::uint8_t byte_at(ByteView data, std::size_t i) noexcept { return std::to_integer<std::uint8_t>(data[i]); }

bool valid_type(std::uint8_t type) noexcept { return (type >= 1 && type <= 8) || type == 15; }

/// MSB-first bit reader for header syntax (f(n), uvlc()).
class BitReader {
 public:
  explicit BitReader(ByteView data) noexcept : data_(data) {}

  /// Reads n <= 32 bits; sets the error flag (and returns 0) past the end.
  std::uint32_t f(int n) noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < n; ++i) {
      if (bit_ >= data_.size() * 8) {
        failed_ = true;
        return 0;
      }
      const std::uint8_t byte = byte_at(data_, bit_ / 8);
      value = (value << 1) | ((byte >> (7 - (bit_ % 8))) & 1u);
      ++bit_;
    }
    return value;
  }
  bool flag() noexcept { return f(1) != 0; }
  std::uint32_t uvlc() noexcept {
    int leading_zeros = 0;
    while (!failed_ && f(1) == 0) {
      if (++leading_zeros >= 32) {
        failed_ = true;
        return 0;
      }
    }
    if (failed_) return 0;
    const std::uint32_t value = leading_zeros == 0 ? 0 : f(leading_zeros);
    return value + ((1u << leading_zeros) - 1u);
  }
  [[nodiscard]] bool ok() const noexcept { return !failed_; }

 private:
  ByteView data_;
  std::size_t bit_ = 0;
  bool failed_ = false;
};

/// Parses one OBU header at the start of `data`; `size_required` demands
/// obu_has_size_field. On success returns the OBU and the bytes consumed.
std::optional<std::pair<Obu, std::size_t>> parse_obu(ByteView data, bool allow_missing_size) {
  if (data.empty()) return std::nullopt;
  const std::uint8_t h = byte_at(data, 0);
  if ((h & 0x80u) != 0) return std::nullopt;  // forbidden bit
  const std::uint8_t type = (h >> 3) & 0x0Fu;
  if (!valid_type(type)) return std::nullopt;
  Obu obu;
  obu.type = static_cast<ObuType>(type);
  obu.has_extension = (h & 0x04u) != 0;
  const bool has_size = (h & 0x02u) != 0;
  std::size_t pos = 1;
  if (obu.has_extension) {
    if (data.size() < 2) return std::nullopt;
    const std::uint8_t e = byte_at(data, 1);
    obu.temporal_id = static_cast<std::uint8_t>(e >> 5);
    obu.spatial_id = static_cast<std::uint8_t>((e >> 3) & 3u);
    pos = 2;
  }
  obu.header = data.first(pos);
  std::size_t payload_size = 0;
  if (has_size) {
    const auto leb = read_leb128(data.subspan(pos));
    if (!leb) return std::nullopt;
    pos += leb->second;
    if (leb->first > data.size() - pos) return std::nullopt;
    payload_size = leb->first;
  } else {
    if (!allow_missing_size) return std::nullopt;
    payload_size = data.size() - pos;
  }
  obu.payload = data.subspan(pos, payload_size);
  return std::pair{obu, pos + payload_size};
}

/// Walks Annex B temporal units; calls `on_obu(header_and_payload_view)` for
/// each OBU (in Annex B form, possibly without size field).
template <class OnObu>
bool walk_annexb(ByteView data, OnObu&& on_obu) {
  std::size_t pos = 0;
  if (data.empty()) return false;
  while (pos < data.size()) {
    const auto tu = read_leb128(data.subspan(pos));
    if (!tu) return false;
    pos += tu->second;
    if (tu->first > data.size() - pos) return false;
    ByteView unit = data.subspan(pos, tu->first);
    pos += tu->first;
    while (!unit.empty()) {
      const auto fu = read_leb128(unit);
      if (!fu) return false;
      unit = unit.subspan(fu->second);
      if (fu->first > unit.size()) return false;
      ByteView frame = unit.first(fu->first);
      unit = unit.subspan(fu->first);
      while (!frame.empty()) {
        const auto ol = read_leb128(frame);
        if (!ol) return false;
        frame = frame.subspan(ol->second);
        if (ol->first > frame.size() || ol->first == 0) return false;
        const ByteView obu_bytes = frame.first(ol->first);
        frame = frame.subspan(ol->first);
        // The OBU may or may not carry its own size field; when it does, the
        // size must agree with obu_length.
        const auto parsed = parse_obu(obu_bytes, true);
        if (!parsed || parsed->second != obu_bytes.size()) return false;
        if (!on_obu(parsed->first)) return false;
      }
    }
  }
  return true;
}

}  // namespace

std::optional<std::pair<std::uint32_t, std::size_t>> read_leb128(ByteView data) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    if (i >= data.size()) return std::nullopt;
    const std::uint8_t byte = byte_at(data, i);
    value |= static_cast<std::uint64_t>(byte & 0x7Fu) << (7 * i);
    if ((byte & 0x80u) == 0) {
      if (value > 0xFFFFFFFFu) return std::nullopt;
      return std::pair{static_cast<std::uint32_t>(value), i + 1};
    }
  }
  return std::nullopt;
}

void write_leb128(Bytes& out, std::uint32_t value) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
    value >>= 7;
    if (value != 0) byte |= 0x80u;
    out.push_back(static_cast<std::byte>(byte));
  } while (value != 0);
}

Result<std::vector<Obu>> split_section5(ByteView data) {
  std::vector<Obu> obus;
  std::size_t pos = 0;
  while (pos < data.size()) {
    const auto parsed = parse_obu(data.subspan(pos), true);
    if (!parsed) return fail(Status::Decode, "malformed AV1 OBU at byte {}", pos);
    // Only the last OBU may omit its size field.
    const bool has_size = (byte_at(parsed->first.header, 0) & 0x02u) != 0;
    if (!has_size && pos + parsed->second != data.size()) {
      return fail(Status::Decode, "AV1 OBU without size field in the middle of a temporal unit");
    }
    obus.push_back(parsed->first);
    pos += parsed->second;
  }
  return obus;
}

bool is_section5(ByteView data, bool allow_last_without_size) {
  if (data.empty()) return false;
  if (allow_last_without_size) return split_section5(data).has_value();
  std::size_t pos = 0;
  while (pos < data.size()) {
    const auto parsed = parse_obu(data.subspan(pos), false);
    if (!parsed) return false;
    pos += parsed->second;
  }
  return true;
}

bool is_annexb(ByteView data) {
  return walk_annexb(data, [](const Obu&) { return true; });
}

void append_obu(Bytes& out, const Obu& obu) {
  std::uint8_t header = std::to_integer<std::uint8_t>(obu.header[0]);
  header |= 0x02u;  // obu_has_size_field
  out.push_back(static_cast<std::byte>(header));
  if (obu.header.size() > 1) out.push_back(obu.header[1]);
  write_leb128(out, static_cast<std::uint32_t>(obu.payload.size()));
  out.insert(out.end(), obu.payload.begin(), obu.payload.end());
}

Result<Bytes> annexb_to_section5(ByteView data) {
  Bytes out;
  out.reserve(data.size() + 16);
  const bool ok = walk_annexb(data, [&](const Obu& obu) {
    append_obu(out, obu);
    return true;
  });
  if (!ok) return fail(Status::Decode, "malformed AV1 Annex B temporal unit");
  return out;
}

Result<std::vector<ByteView>> split_temporal_units(ByteView data) {
  std::vector<ByteView> units;
  std::size_t pos = 0;
  std::size_t start = 0;
  while (pos < data.size()) {
    const auto parsed = parse_obu(data.subspan(pos), true);
    if (!parsed) return fail(Status::Decode, "malformed AV1 OBU at byte {}", pos);
    const bool has_size = (byte_at(parsed->first.header, 0) & 0x02u) != 0;
    if (!has_size && pos + parsed->second != data.size()) {
      return fail(Status::Decode, "AV1 OBU without size field in the middle of a temporal unit");
    }
    if (parsed->first.type == ObuType::TemporalDelimiter && pos > start) {
      units.push_back(data.subspan(start, pos - start));
      start = pos;
    }
    pos += parsed->second;
  }
  if (pos > start) units.push_back(data.subspan(start, pos - start));
  return units;
}

Result<Bytes> strip_temporal_delimiters(ByteView data) {
  CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(data));
  Bytes out;
  out.reserve(data.size());
  for (const Obu& obu : obus) {
    if (obu.type == ObuType::TemporalDelimiter || obu.type == ObuType::Padding) continue;
    append_obu(out, obu);
  }
  return out;
}

Result<SequenceHeader> parse_sequence_header(ByteView payload) {
  BitReader r(payload);
  SequenceHeader s;
  s.profile = static_cast<std::uint8_t>(r.f(3));
  if (s.profile > 2) return fail(Status::Decode, "unsupported AV1 profile {}", s.profile);
  s.still_picture = r.flag();
  s.reduced_still_picture_header = r.flag();
  if (s.reduced_still_picture_header && !s.still_picture) {
    return fail(Status::Decode, "invalid AV1 sequence header (reduced header without still picture)");
  }
  bool decoder_model_info_present = false;
  std::uint32_t buffer_delay_length = 0;
  if (s.reduced_still_picture_header) {
    s.level_idx0 = static_cast<std::uint8_t>(r.f(5));
  } else {
    const bool timing_info_present = r.flag();
    if (timing_info_present) {
      r.f(32);  // num_units_in_display_tick
      r.f(32);  // time_scale
      if (r.flag()) r.uvlc();  // equal_picture_interval -> num_ticks_per_picture_minus_1
      decoder_model_info_present = r.flag();
      if (decoder_model_info_present) {
        buffer_delay_length = r.f(5) + 1;
        r.f(32);  // num_units_in_decoding_tick
        r.f(5);   // buffer_removal_time_length_minus_1
        r.f(5);   // frame_presentation_time_length_minus_1
      }
    }
    const bool initial_display_delay_present = r.flag();
    s.operating_points = r.f(5) + 1;
    for (std::uint32_t i = 0; i < s.operating_points; ++i) {
      r.f(12);  // operating_point_idc
      const auto level = static_cast<std::uint8_t>(r.f(5));
      const std::uint8_t tier = level > 7 ? static_cast<std::uint8_t>(r.f(1)) : 0;
      if (decoder_model_info_present && r.flag()) {
        r.f(static_cast<int>(buffer_delay_length));  // decoder_buffer_delay
        r.f(static_cast<int>(buffer_delay_length));  // encoder_buffer_delay
        r.f(1);                                      // low_delay_mode_flag
      }
      bool delay_present = false;
      std::uint8_t delay = 0;
      if (initial_display_delay_present) {
        delay_present = r.flag();
        if (delay_present) delay = static_cast<std::uint8_t>(r.f(4));
      }
      if (i == 0) {
        s.level_idx0 = level;
        s.tier0 = tier;
        s.initial_display_delay_present0 = delay_present;
        s.initial_display_delay_minus_one0 = delay;
      }
    }
  }
  const int width_bits = static_cast<int>(r.f(4)) + 1;
  const int height_bits = static_cast<int>(r.f(4)) + 1;
  s.max_frame_width = r.f(width_bits) + 1;
  s.max_frame_height = r.f(height_bits) + 1;
  bool frame_id_numbers_present = false;
  if (!s.reduced_still_picture_header) frame_id_numbers_present = r.flag();
  if (frame_id_numbers_present) {
    r.f(4);  // delta_frame_id_length_minus_2
    r.f(3);  // additional_frame_id_length_minus_1
  }
  s.use_128x128_superblock = r.flag();
  r.f(1);  // enable_filter_intra
  r.f(1);  // enable_intra_edge_filter
  if (!s.reduced_still_picture_header) {
    r.f(1);  // enable_interintra_compound
    r.f(1);  // enable_masked_compound
    r.f(1);  // enable_warped_motion
    r.f(1);  // enable_dual_filter
    const bool enable_order_hint = r.flag();
    if (enable_order_hint) {
      r.f(1);  // enable_jnt_comp
      r.f(1);  // enable_ref_frame_mvs
    }
    std::uint32_t force_screen_content_tools = 2;
    if (!r.flag()) force_screen_content_tools = r.f(1);  // seq_choose_screen_content_tools
    if (force_screen_content_tools > 0) {
      if (!r.flag()) r.f(1);  // seq_choose_integer_mv / seq_force_integer_mv
    }
    if (enable_order_hint) r.f(3);  // order_hint_bits_minus_1
  }
  s.enable_superres = r.flag();
  s.enable_cdef = r.flag();
  s.enable_restoration = r.flag();
  // color_config()
  const bool high_bitdepth = r.flag();
  if (s.profile == 2 && high_bitdepth) {
    s.bit_depth = r.flag() ? 12 : 10;
  } else {
    s.bit_depth = high_bitdepth ? 10 : 8;
  }
  s.monochrome = s.profile == 1 ? false : r.flag();
  s.color_description_present = r.flag();
  if (s.color_description_present) {
    s.color_primaries = static_cast<std::uint8_t>(r.f(8));
    s.transfer_characteristics = static_cast<std::uint8_t>(r.f(8));
    s.matrix_coefficients = static_cast<std::uint8_t>(r.f(8));
  }
  if (s.monochrome) {
    s.full_range = r.flag();
    s.subsampling_x = s.subsampling_y = 1;
    s.chroma_sample_position = 0;
  } else if (s.color_primaries == 1 && s.transfer_characteristics == 13 && s.matrix_coefficients == 0) {
    s.full_range = true;
    s.subsampling_x = s.subsampling_y = 0;
  } else {
    s.full_range = r.flag();
    if (s.profile == 0) {
      s.subsampling_x = s.subsampling_y = 1;
    } else if (s.profile == 1) {
      s.subsampling_x = s.subsampling_y = 0;
    } else if (s.bit_depth == 12) {
      s.subsampling_x = static_cast<std::uint8_t>(r.f(1));
      s.subsampling_y = s.subsampling_x != 0 ? static_cast<std::uint8_t>(r.f(1)) : 0;
    } else {
      s.subsampling_x = 1;
      s.subsampling_y = 0;
    }
    if (s.subsampling_x != 0 && s.subsampling_y != 0) s.chroma_sample_position = static_cast<std::uint8_t>(r.f(2));
  }
  if (!s.monochrome) s.separate_uv_delta_q = r.flag();
  s.film_grain_params_present = r.flag();
  if (!r.ok()) return fail(Status::Decode, "truncated AV1 sequence header");
  if (s.profile == 1 && (s.subsampling_x != 0 || s.subsampling_y != 0)) {
    return fail(Status::Decode, "invalid AV1 sequence header (profile 1 must be 4:4:4)");
  }
  return s;
}

Result<std::optional<Bytes>> find_sequence_header_obu(ByteView data) {
  CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(data));
  for (const Obu& obu : obus) {
    if (obu.type != ObuType::SequenceHeader) continue;
    Bytes out;
    append_obu(out, obu);
    return std::optional<Bytes>(std::move(out));
  }
  return std::optional<Bytes>();
}

Result<Bytes> make_av1c(ByteView sequence_header_obu) {
  CVT_TRY_ASSIGN(const std::vector<Obu> obus, split_section5(sequence_header_obu));
  if (obus.size() != 1 || obus[0].type != ObuType::SequenceHeader) {
    return fail(Status::InvalidArgument, "av1C needs exactly one sequence header OBU");
  }
  CVT_TRY_ASSIGN(const SequenceHeader s, parse_sequence_header(obus[0].payload));
  Bytes out;
  out.push_back(std::byte{0x81});  // marker 1, version 1
  const unsigned profile = s.profile;
  const unsigned level = s.level_idx0;
  const unsigned tier = s.tier0;
  const unsigned ssx = s.subsampling_x;
  const unsigned ssy = s.subsampling_y;
  const unsigned position = s.chroma_sample_position;
  out.push_back(static_cast<std::byte>((profile << 5) | (level & 0x1Fu)));
  const std::uint8_t b2 = static_cast<std::uint8_t>(
      ((tier & 1u) << 7) | ((s.bit_depth > 8 ? 1u : 0u) << 6) | ((s.bit_depth == 12 ? 1u : 0u) << 5) |
      ((s.monochrome ? 1u : 0u) << 4) | ((ssx & 1u) << 3) | ((ssy & 1u) << 2) | (position & 3u));
  out.push_back(static_cast<std::byte>(b2));
  if (s.initial_display_delay_present0) {
    out.push_back(static_cast<std::byte>(0x10u | (s.initial_display_delay_minus_one0 & 0x0Fu)));
  } else {
    out.push_back(std::byte{0});
  }
  append_obu(out, obus[0]);
  return out;
}

Result<Av1Config> parse_av1c(ByteView data) {
  if (data.size() < 4) return fail(Status::Decode, "AV1 configuration record (av1C) is too short");
  const std::uint8_t b0 = byte_at(data, 0);
  if ((b0 & 0x80u) == 0 || (b0 & 0x7Fu) != 1) return fail(Status::Decode, "unsupported AV1 configuration record version");
  Av1Config c;
  const std::uint8_t b1 = byte_at(data, 1);
  const std::uint8_t b2 = byte_at(data, 2);
  const std::uint8_t b3 = byte_at(data, 3);
  c.profile = static_cast<std::uint8_t>(b1 >> 5);
  c.level_idx0 = static_cast<std::uint8_t>(b1 & 0x1Fu);
  c.tier0 = static_cast<std::uint8_t>(b2 >> 7);
  c.high_bitdepth = (b2 & 0x40u) != 0;
  c.twelve_bit = (b2 & 0x20u) != 0;
  c.monochrome = (b2 & 0x10u) != 0;
  c.subsampling_x = static_cast<std::uint8_t>((b2 >> 3) & 1u);
  c.subsampling_y = static_cast<std::uint8_t>((b2 >> 2) & 1u);
  c.chroma_sample_position = static_cast<std::uint8_t>(b2 & 3u);
  if ((b3 & 0x10u) != 0) c.initial_presentation_delay_minus_one = static_cast<std::uint8_t>(b3 & 0x0Fu);
  const ByteView obus = data.subspan(4);
  if (!obus.empty()) {
    CVT_TRY(split_section5(obus));  // validates structure
  }
  c.config_obus.assign(obus.begin(), obus.end());
  return c;
}

}  // namespace cvt::av1
