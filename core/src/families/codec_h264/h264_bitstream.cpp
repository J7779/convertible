// Convertible core - codec_h264 family: H.264 bitstream helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_h264/h264_bitstream.hpp"

#include <algorithm>
#include <format>
#include <numeric>

namespace cvt::h264 {

namespace {

[[nodiscard]] std::uint8_t byte_at(ByteView data, std::size_t index) noexcept {
  return static_cast<std::uint8_t>(std::to_integer<unsigned>(data[index]));
}

/// Index of the next 00 00 01 at or after `from`, or data.size().
[[nodiscard]] std::size_t find_start_code(ByteView data, std::size_t from) noexcept {
  std::size_t i = from;
  while (i + 2 < data.size()) {
    const std::uint8_t third = byte_at(data, i + 2);
    if (third > 1) {
      i += 3;  // neither 00 nor 01: no start code can begin at i, i+1 or i+2
    } else if (third == 1 && byte_at(data, i) == 0 && byte_at(data, i + 1) == 0) {
      return i;
    } else {
      ++i;
    }
  }
  return data.size();
}

[[nodiscard]] bool high_profile_syntax(std::uint32_t profile) noexcept {
  switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
      return true;
    default:
      return false;
  }
}

/// Skip one scaling_list() of `size` coefficients (7.3.2.1.1.1).
void skip_scaling_list(BitReader& reader, int size) noexcept {
  std::int32_t last = 8;
  std::int32_t next = 8;
  for (int j = 0; j < size && !reader.overrun(); ++j) {
    if (next != 0) {
      const std::int32_t delta = reader.se();
      next = ((last + delta) % 256 + 256) % 256;
    }
    last = next == 0 ? last : next;
  }
}

/// hrd_parameters() (E.1.2). False when the values are out of range.
[[nodiscard]] bool skip_hrd(BitReader& reader) noexcept {
  const std::uint32_t cpb_count = reader.ue() + 1;
  if (cpb_count > 32) return false;
  reader.skip(8);  // bit_rate_scale, cpb_size_scale
  for (std::uint32_t i = 0; i < cpb_count; ++i) {
    (void)reader.ue();  // bit_rate_value_minus1
    (void)reader.ue();  // cpb_size_value_minus1
    reader.skip(1);     // cbr_flag
  }
  reader.skip(20);  // four 5-bit delay lengths
  return !reader.overrun();
}

void parse_vui(BitReader& reader, Sps& sps) noexcept {
  sps.vui_present = true;
  if (reader.flag()) {  // aspect_ratio_info_present_flag
    static constexpr std::uint8_t table[17][2] = {{0, 0},   {1, 1},   {12, 11}, {10, 11}, {16, 11}, {40, 33},
                                                  {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11},
                                                  {64, 33}, {160, 99}, {4, 3},  {3, 2},   {2, 1}};
    const std::uint32_t idc = reader.bits(8);
    if (idc == 255) {
      sps.sar_num = reader.bits(16);
      sps.sar_den = reader.bits(16);
    } else if (idc < 17) {
      sps.sar_num = table[idc][0];
      sps.sar_den = table[idc][1];
    }
  }
  if (reader.flag()) reader.skip(1);  // overscan_info_present_flag, overscan_appropriate_flag
  if (reader.flag()) {                // video_signal_type_present_flag
    reader.skip(3);                   // video_format
    sps.video_full_range = reader.flag();
    if (reader.flag()) {
      sps.colour_description_present = true;
      sps.colour_primaries = static_cast<std::uint8_t>(reader.bits(8));
      sps.transfer_characteristics = static_cast<std::uint8_t>(reader.bits(8));
      sps.matrix_coefficients = static_cast<std::uint8_t>(reader.bits(8));
    }
  }
  if (reader.flag()) {  // chroma_loc_info_present_flag
    sps.chroma_loc_present = true;
    sps.chroma_sample_loc_type = reader.ue();
    (void)reader.ue();  // bottom field
  }
  if (reader.flag()) {  // timing_info_present_flag
    sps.num_units_in_tick = reader.bits(32);
    sps.time_scale = reader.bits(32);
    sps.fixed_frame_rate = reader.flag();
    sps.timing_info_present = !reader.overrun() && sps.num_units_in_tick != 0 && sps.time_scale != 0;
  }
  const bool nal_hrd = reader.flag();
  if (nal_hrd && !skip_hrd(reader)) {
    sps.vui_damaged = true;
    return;
  }
  const bool vcl_hrd = reader.flag();
  if (vcl_hrd && !skip_hrd(reader)) {
    sps.vui_damaged = true;
    return;
  }
  if (nal_hrd || vcl_hrd) reader.skip(1);  // low_delay_hrd_flag
  reader.skip(1);                          // pic_struct_present_flag
  if (reader.flag()) {                     // bitstream_restriction_flag
    reader.skip(1);
    (void)reader.ue();
    (void)reader.ue();
    (void)reader.ue();
    (void)reader.ue();
    const std::uint32_t reorder = reader.ue();
    const std::uint32_t buffering = reader.ue();
    if (!reader.overrun() && reorder <= 16 && buffering <= 16) {
      sps.bitstream_restriction = true;
      sps.max_num_reorder_frames = reorder;
      sps.max_dec_frame_buffering = buffering;
    }
  }
  if (reader.overrun()) sps.vui_damaged = true;
}

}  // namespace

// ----------------------------------------------------------------------------- framing

bool starts_with_start_code(ByteView data) noexcept {
  if (data.size() >= 3 && byte_at(data, 0) == 0 && byte_at(data, 1) == 0 && byte_at(data, 2) == 1) return true;
  return data.size() >= 4 && byte_at(data, 0) == 0 && byte_at(data, 1) == 0 && byte_at(data, 2) == 0 &&
         byte_at(data, 3) == 1;
}

std::vector<ByteView> split_annexb(ByteView stream) {
  std::vector<ByteView> nals;
  std::size_t start = find_start_code(stream, 0);
  while (start < stream.size()) {
    const std::size_t begin = start + 3;
    const std::size_t next = find_start_code(stream, begin);
    std::size_t end = next;
    while (end > begin && byte_at(stream, end - 1) == 0) --end;  // trailing_zero_8bits / 4-byte start codes
    if (end > begin) nals.push_back(stream.subspan(begin, end - begin));
    start = next;
  }
  return nals;
}

std::vector<ByteView> split_access_units(ByteView stream) {
  std::vector<ByteView> units;
  std::size_t unit_start = stream.size();
  bool unit_has_vcl = false;
  std::size_t start = find_start_code(stream, 0);
  while (start < stream.size()) {
    const std::size_t begin = start + 3;
    const std::size_t next = find_start_code(stream, begin);
    const ByteView nal = stream.subspan(begin, next - begin);
    const std::uint8_t type = nal_type(nal);
    bool boundary = false;
    if (type == nal_aud) {
      boundary = true;
    } else if (type == nal_sei || type == nal_sps || type == nal_pps || (type >= 14 && type <= 18)) {
      boundary = unit_has_vcl;
    } else if (is_vcl(type)) {
      if (unit_has_vcl) {
        // first_mb_in_slice is the first ue(v) of the slice header.
        const Bytes head = nal_to_rbsp(nal.first(std::min<std::size_t>(nal.size(), 8)));
        BitReader reader(head);
        boundary = reader.ue() == 0 && !reader.overrun();
      }
    }
    if (boundary && unit_start < start) {
      units.push_back(stream.subspan(unit_start, start - unit_start));
      unit_start = start;
      unit_has_vcl = false;
    }
    if (unit_start == stream.size()) unit_start = start;
    if (is_vcl(type)) unit_has_vcl = true;
    start = next;
  }
  if (unit_start < stream.size()) units.push_back(stream.subspan(unit_start));
  return units;
}

Result<std::vector<ByteView>> split_length_prefixed(ByteView sample, int length_size) {
  if (length_size != 1 && length_size != 2 && length_size != 4) {
    return fail(Status::Decode, "invalid H.264 NAL length size {}", length_size);
  }
  std::vector<ByteView> nals;
  std::size_t pos = 0;
  const auto width = static_cast<std::size_t>(length_size);
  while (pos < sample.size()) {
    if (sample.size() - pos < width) return fail(Status::Decode, "truncated H.264 NAL length prefix");
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < width; ++i) length = (length << 8) | byte_at(sample, pos + i);
    pos += width;
    if (length > sample.size() - pos) return fail(Status::Decode, "H.264 NAL unit runs past the end of the sample");
    if (length > 0) nals.push_back(sample.subspan(pos, static_cast<std::size_t>(length)));
    pos += static_cast<std::size_t>(length);
  }
  return nals;
}

Bytes to_annexb(std::span<const ByteView> nals) {
  std::size_t total = 0;
  for (const ByteView nal : nals) total += nal.size() + 4;
  Bytes out;
  out.reserve(total);
  for (const ByteView nal : nals) {
    if (nal.empty()) continue;
    out.insert(out.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

Result<Bytes> to_length_prefixed(std::span<const ByteView> nals, int length_size) {
  if (length_size != 1 && length_size != 2 && length_size != 4) {
    return fail(Status::InvalidArgument, "invalid H.264 NAL length size {}", length_size);
  }
  const std::uint64_t limit = length_size == 4 ? 0xFFFFFFFFull : (1ull << (8 * length_size)) - 1;
  std::size_t total = 0;
  for (const ByteView nal : nals) total += nal.size() + static_cast<std::size_t>(length_size);
  Bytes out;
  out.reserve(total);
  for (const ByteView nal : nals) {
    if (nal.empty()) continue;
    const std::uint64_t length = nal.size();
    if (length > limit) return fail(Status::Encode, "H.264 NAL unit too long for a {}-byte length", length_size);
    for (int i = length_size - 1; i >= 0; --i) out.push_back(static_cast<std::byte>((length >> (8 * i)) & 0xFF));
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

Result<Bytes> annexb_to_length_prefixed(ByteView stream, int length_size) {
  const std::vector<ByteView> nals = split_annexb(stream);
  return to_length_prefixed(nals, length_size);
}

Result<Bytes> length_prefixed_to_annexb(ByteView sample, int length_size) {
  CVT_TRY_ASSIGN(const std::vector<ByteView> nals, split_length_prefixed(sample, length_size));
  return to_annexb(nals);
}

Bytes nal_to_rbsp(ByteView nal) {
  Bytes rbsp;
  if (nal.size() <= 1) return rbsp;
  rbsp.reserve(nal.size() - 1);
  int zeros = 0;
  for (std::size_t i = 1; i < nal.size(); ++i) {
    const std::uint8_t value = byte_at(nal, i);
    if (zeros >= 2 && value == 3) {
      zeros = 0;  // emulation_prevention_three_byte
      continue;
    }
    zeros = value == 0 ? zeros + 1 : 0;
    rbsp.push_back(nal[i]);
  }
  return rbsp;
}

// ----------------------------------------------------------------------------- BitReader

std::uint32_t BitReader::bits(int count) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < count; ++i) {
    std::uint32_t bit = 0;
    if (position_ < size_bits()) {
      const auto byte = static_cast<std::size_t>(position_ >> 3);
      bit = (std::to_integer<std::uint32_t>(data_[byte]) >> (7 - (position_ & 7))) & 1u;
    } else {
      overrun_ = true;
    }
    ++position_;
    value = (value << 1) | bit;
  }
  return value;
}

std::uint32_t BitReader::ue() noexcept {
  int zeros = 0;
  while (!flag()) {
    if (overrun_ || ++zeros > 31) {
      overrun_ = true;
      return 0;
    }
  }
  if (zeros == 0) return 0;
  return ((1u << zeros) - 1u) + bits(zeros);
}

std::int32_t BitReader::se() noexcept {
  const std::uint32_t code = ue();
  const auto magnitude = static_cast<std::int64_t>((static_cast<std::uint64_t>(code) + 1) / 2);
  return static_cast<std::int32_t>((code & 1u) != 0 ? magnitude : -magnitude);
}

void BitReader::skip(std::uint64_t count) noexcept {
  if (count > size_bits() - std::min(position_, size_bits())) overrun_ = true;
  position_ += std::min<std::uint64_t>(count, size_bits() + 64);
}

// ----------------------------------------------------------------------------- SPS

ir::Rational Sps::frame_rate() const noexcept {
  if (!timing_info_present || num_units_in_tick == 0 || time_scale == 0) return {0, 1};
  const std::int64_t num = time_scale;
  const std::int64_t den = 2 * static_cast<std::int64_t>(num_units_in_tick);
  const std::int64_t divisor = std::gcd(num, den);
  return {num / divisor, den / divisor};
}

ir::ColorInfo Sps::color_info() const noexcept {
  ir::ColorInfo info;
  info.range = video_full_range ? ir::ColorRange::Full : ir::ColorRange::Limited;
  const auto from_code = [](std::uint8_t code) -> std::optional<ir::ColorMatrix> {
    switch (code) {
      case 1: return ir::ColorMatrix::BT709;
      case 5: case 6: return ir::ColorMatrix::BT601;
      case 9: case 10: return ir::ColorMatrix::BT2020;
      default: return std::nullopt;
    }
  };
  if (const auto matrix = from_code(matrix_coefficients)) {
    info.matrix = *matrix;
  } else if (const auto primaries = from_code(colour_primaries)) {
    info.matrix = *primaries;
  } else {
    info.matrix = height >= 720 ? ir::ColorMatrix::BT709 : ir::ColorMatrix::BT601;
  }
  switch (chroma_loc_present ? chroma_sample_loc_type : 0) {
    case 1: info.siting = ir::ChromaSiting::Center; break;
    case 2: info.siting = ir::ChromaSiting::TopLeft; break;
    default: info.siting = ir::ChromaSiting::Left; break;
  }
  return info;
}

std::string Sps::profile_name() const {
  const bool set1 = (constraint_flags & 0x40) != 0;
  const bool set3 = (constraint_flags & 0x10) != 0;
  switch (profile_idc) {
    case 66: return set1 ? "Constrained Baseline" : "Baseline";
    case 77: return "Main";
    case 88: return "Extended";
    case 100: return "High";
    case 110: return set3 ? "High 10 Intra" : "High 10";
    case 122: return set3 ? "High 4:2:2 Intra" : "High 4:2:2";
    case 244: return set3 ? "High 4:4:4 Intra" : "High 4:4:4 Predictive";
    case 44: return "CAVLC 4:4:4 Intra";
    case 83: case 86: return "Scalable (SVC)";
    case 118: case 128: case 134: return "Multiview (MVC)";
    default: return std::format("profile {}", profile_idc);
  }
}

Result<Sps> parse_sps(ByteView nal) {
  if (nal_type(nal) != nal_sps) return fail(Status::Decode, "not an H.264 sequence parameter set");
  const Bytes rbsp = nal_to_rbsp(nal);
  BitReader reader(rbsp);
  Sps sps;
  sps.profile_idc = static_cast<std::uint8_t>(reader.bits(8));
  sps.constraint_flags = static_cast<std::uint8_t>(reader.bits(8));
  sps.level_idc = static_cast<std::uint8_t>(reader.bits(8));
  sps.id = reader.ue();
  if (sps.id > 31) return fail(Status::Decode, "invalid H.264 sequence parameter set id {}", sps.id);
  if (high_profile_syntax(sps.profile_idc)) {
    sps.chroma_format_idc = reader.ue();
    if (sps.chroma_format_idc > 3) return fail(Status::Decode, "invalid H.264 chroma format {}", sps.chroma_format_idc);
    if (sps.chroma_format_idc == 3) sps.separate_colour_plane = reader.flag();
    sps.bit_depth_luma = reader.ue() + 8;
    sps.bit_depth_chroma = reader.ue() + 8;
    if (sps.bit_depth_luma > 14 || sps.bit_depth_chroma > 14) return fail(Status::Decode, "invalid H.264 bit depth");
    sps.lossless_bypass = reader.flag();
    if (reader.flag()) {  // seq_scaling_matrix_present_flag
      const int lists = sps.chroma_format_idc != 3 ? 8 : 12;
      for (int i = 0; i < lists; ++i) {
        if (reader.flag()) skip_scaling_list(reader, i < 6 ? 16 : 64);
      }
    }
  }
  const std::uint32_t log2_max_frame_num = reader.ue() + 4;
  if (log2_max_frame_num > 16) return fail(Status::Decode, "invalid H.264 log2_max_frame_num");
  const std::uint32_t poc_type = reader.ue();
  if (poc_type == 0) {
    if (reader.ue() > 12) return fail(Status::Decode, "invalid H.264 log2_max_pic_order_cnt_lsb");
  } else if (poc_type == 1) {
    reader.skip(1);
    (void)reader.se();
    (void)reader.se();
    const std::uint32_t cycle = reader.ue();
    if (cycle > 255) return fail(Status::Decode, "invalid H.264 picture order count cycle");
    for (std::uint32_t i = 0; i < cycle && !reader.overrun(); ++i) (void)reader.se();
  } else if (poc_type != 2) {
    return fail(Status::Decode, "invalid H.264 picture order count type {}", poc_type);
  }
  sps.max_num_ref_frames = reader.ue();
  reader.skip(1);  // gaps_in_frame_num_value_allowed_flag
  const std::uint32_t width_mbs = reader.ue() + 1;
  const std::uint32_t height_map_units = reader.ue() + 1;
  sps.frame_mbs_only = reader.flag();
  if (!sps.frame_mbs_only) sps.mb_adaptive_frame_field = reader.flag();
  reader.skip(1);  // direct_8x8_inference_flag
  if (reader.overrun()) return fail(Status::Decode, "truncated H.264 sequence parameter set");
  // 1024 macroblocks = 16384 samples; far beyond any level, but keeps the arithmetic small.
  if (width_mbs > 1024 || height_map_units > 1024) {
    return fail(Status::Decode, "H.264 picture size {}x{} macroblocks is out of range", width_mbs, height_map_units);
  }
  sps.coded_width = width_mbs * 16;
  sps.coded_height = height_map_units * 16 * (sps.frame_mbs_only ? 1u : 2u);

  std::uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  if (reader.flag()) {
    crop_left = reader.ue();
    crop_right = reader.ue();
    crop_top = reader.ue();
    crop_bottom = reader.ue();
  }
  const std::uint32_t chroma_array_type = sps.separate_colour_plane ? 0 : sps.chroma_format_idc;
  const std::uint32_t sub_width = chroma_array_type == 1 || chroma_array_type == 2 ? 2 : 1;
  const std::uint32_t sub_height = chroma_array_type == 1 ? 2 : 1;
  const std::uint32_t unit_x = chroma_array_type == 0 ? 1 : sub_width;
  const std::uint32_t unit_y = (chroma_array_type == 0 ? 1 : sub_height) * (sps.frame_mbs_only ? 1u : 2u);
  const std::uint64_t crop_x = static_cast<std::uint64_t>(unit_x) * (static_cast<std::uint64_t>(crop_left) + crop_right);
  const std::uint64_t crop_y = static_cast<std::uint64_t>(unit_y) * (static_cast<std::uint64_t>(crop_top) + crop_bottom);
  if (crop_x >= sps.coded_width || crop_y >= sps.coded_height) {
    return fail(Status::Decode, "invalid H.264 cropping rectangle");
  }
  sps.crop_left = crop_left * unit_x;
  sps.crop_right = crop_right * unit_x;
  sps.crop_top = crop_top * unit_y;
  sps.crop_bottom = crop_bottom * unit_y;
  sps.width = sps.coded_width - static_cast<std::uint32_t>(crop_x);
  sps.height = sps.coded_height - static_cast<std::uint32_t>(crop_y);

  if (reader.flag()) parse_vui(reader, sps);
  if (reader.overrun() && !sps.vui_present) return fail(Status::Decode, "truncated H.264 sequence parameter set");
  return sps;
}

std::optional<std::string> unsupported_reason(const Sps& sps) {
  if (sps.chroma_format_idc == 0) return "monochrome (4:0:0) H.264 video is not supported yet";
  if (sps.bit_depth_luma != 8 || sps.bit_depth_chroma != 8) {
    return std::format("H.264 {} profile with {}-bit samples is not supported yet", sps.profile_name(),
                       std::max(sps.bit_depth_luma, sps.bit_depth_chroma));
  }
  if (sps.chroma_format_idc == 2) return std::format("H.264 {} profile (4:2:2 chroma) is not supported yet", sps.profile_name());
  if (sps.chroma_format_idc == 3) return std::format("H.264 {} profile (4:4:4 chroma) is not supported yet", sps.profile_name());
  if (sps.lossless_bypass) return "lossless H.264 (transform bypass) is not supported yet";
  switch (sps.profile_idc) {
    case 66: case 77: case 100:
      return std::nullopt;
    case 88:
      if ((sps.constraint_flags & 0xC0) != 0) return std::nullopt;  // Extended streams that are also Baseline/Main
      return "H.264 Extended profile video is not supported yet";
    default:
      return std::format("H.264 {} profile video is not supported yet", sps.profile_name());
  }
}

// ----------------------------------------------------------------------------- avcC

bool is_avcc(ByteView extradata) noexcept { return !extradata.empty() && byte_at(extradata, 0) == 1; }

Result<AvcConfig> parse_avcc(ByteView record) {
  util::ByteReader reader(record);
  if (record.size() < 7 || reader.u8() != 1) return fail(Status::Decode, "invalid H.264 configuration record (avcC)");
  AvcConfig config;
  config.profile_idc = reader.u8();
  config.profile_compatibility = reader.u8();
  config.level_idc = reader.u8();
  const std::uint8_t length_byte = reader.u8();
  config.length_size = (length_byte & 3) + 1;
  if (config.length_size == 3) return fail(Status::Decode, "invalid H.264 NAL length size 3 in avcC");
  const auto read_sets = [&reader](std::size_t count, std::vector<Bytes>& into) -> Result<void> {
    for (std::size_t i = 0; i < count; ++i) {
      if (reader.remaining() < 2) return fail(Status::Decode, "truncated H.264 configuration record (avcC)");
      const std::uint16_t length = reader.u16be();
      if (reader.remaining() < length) return fail(Status::Decode, "truncated H.264 configuration record (avcC)");
      const ByteView set = reader.bytes(length);
      if (!set.empty()) into.emplace_back(set.begin(), set.end());
    }
    return {};
  };
  CVT_TRY(read_sets(reader.u8() & 0x1Fu, config.sps));
  if (reader.remaining() < 1) return fail(Status::Decode, "truncated H.264 configuration record (avcC)");
  CVT_TRY(read_sets(reader.u8(), config.pps));
  if (high_profile_syntax(config.profile_idc) && reader.remaining() >= 4) {
    config.chroma_format_idc = reader.u8() & 3u;
    config.bit_depth_luma = (reader.u8() & 7u) + 8;
    config.bit_depth_chroma = (reader.u8() & 7u) + 8;
    // Some writers put garbage here; the extension is advisory, so a damaged
    // one is ignored rather than rejected.
    if (!read_sets(reader.u8(), config.sps_ext)) config.sps_ext.clear();
  }
  return config;
}

Result<Bytes> build_avcc(std::span<const ByteView> sps, std::span<const ByteView> pps, int length_size) {
  if (sps.empty() || pps.empty()) return fail(Status::Encode, "an avcC record needs at least one SPS and one PPS");
  if (sps.size() > 31 || pps.size() > 255) return fail(Status::Encode, "too many H.264 parameter sets for avcC");
  if (length_size != 1 && length_size != 2 && length_size != 4) {
    return fail(Status::InvalidArgument, "invalid H.264 NAL length size {}", length_size);
  }
  if (sps[0].size() < 4) return fail(Status::Encode, "H.264 sequence parameter set is too short");
  CVT_TRY_ASSIGN(const Sps first, parse_sps(sps[0]));
  util::ByteWriter out;
  out.u8(1);
  out.u8(byte_at(sps[0], 1));
  out.u8(byte_at(sps[0], 2));
  out.u8(byte_at(sps[0], 3));
  out.u8(static_cast<std::uint8_t>(0xFC | (length_size - 1)));
  out.u8(static_cast<std::uint8_t>(0xE0 | sps.size()));
  for (const ByteView set : sps) {
    if (set.size() > 0xFFFF) return fail(Status::Encode, "H.264 parameter set too long for avcC");
    out.u16be(static_cast<std::uint16_t>(set.size()));
    out.bytes(set);
  }
  out.u8(static_cast<std::uint8_t>(pps.size()));
  for (const ByteView set : pps) {
    if (set.size() > 0xFFFF) return fail(Status::Encode, "H.264 parameter set too long for avcC");
    out.u16be(static_cast<std::uint16_t>(set.size()));
    out.bytes(set);
  }
  if (first.profile_idc == 100 || first.profile_idc == 110 || first.profile_idc == 122 || first.profile_idc == 144) {
    out.u8(static_cast<std::uint8_t>(0xFC | first.chroma_format_idc));
    out.u8(static_cast<std::uint8_t>(0xF8 | (first.bit_depth_luma - 8)));
    out.u8(static_cast<std::uint8_t>(0xF8 | (first.bit_depth_chroma - 8)));
    out.u8(0);
  }
  return out.take();
}

}  // namespace cvt::h264
