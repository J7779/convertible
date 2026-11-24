// Convertible core - codec_hevc family: NAL units, parameter sets, hvcC,
// access units and picture order counts (ITU-T H.265, ISO/IEC 14496-15).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "families/codec_hevc/hevc_api.hpp"
#include "families/codec_hevc/hevc_internal.hpp"
#include "util/bytes.hpp"

namespace cvt::hevc {

namespace {

[[nodiscard]] std::uint8_t byte_at(ByteView data, std::size_t i) noexcept {
  return std::to_integer<std::uint8_t>(data[i]);
}

/// Position of the next start code (00 00 01) at or after `from`; size() when none.
[[nodiscard]] std::size_t find_start_code(ByteView data, std::size_t from) noexcept {
  for (std::size_t i = from; i + 2 < data.size(); ++i) {
    if (byte_at(data, i + 2) > 1) {
      i += 2;  // neither of the next two positions can start a start code
      continue;
    }
    if (byte_at(data, i) == 0 && byte_at(data, i + 1) == 0 && byte_at(data, i + 2) == 1) return i;
  }
  return data.size();
}

[[nodiscard]] Result<void> parse_profile_tier_level(BitReader& r, unsigned max_sub_layers_minus1,
                                                    ProfileTierLevel& ptl) {
  ptl.profile_space = static_cast<std::uint8_t>(r.bits(2));
  ptl.tier = r.flag();
  ptl.profile_idc = static_cast<std::uint8_t>(r.bits(5));
  ptl.compatibility_flags = r.bits(32);
  ptl.constraint_flags = r.bits64(48);
  ptl.level_idc = static_cast<std::uint8_t>(r.bits(8));
  std::array<bool, 8> profile_present{};
  std::array<bool, 8> level_present{};
  for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
    profile_present[i] = r.flag();
    level_present[i] = r.flag();
  }
  if (max_sub_layers_minus1 > 0) {
    for (unsigned i = max_sub_layers_minus1; i < 8; ++i) r.skip(2);
  }
  for (unsigned i = 0; i < max_sub_layers_minus1; ++i) {
    if (profile_present[i]) r.skip(88);
    if (level_present[i]) r.skip(8);
  }
  if (!r.ok()) return fail(Status::Decode, "the HEVC profile_tier_level is truncated");
  return {};
}

[[nodiscard]] Result<void> skip_scaling_list_data(BitReader& r) {
  for (unsigned size_id = 0; size_id < 4; ++size_id) {
    for (unsigned matrix_id = 0; matrix_id < 6; matrix_id += (size_id == 3) ? 3 : 1) {
      if (!r.flag()) {
        if (r.ue() > matrix_id) return fail(Status::Decode, "invalid HEVC scaling list prediction");
      } else {
        const unsigned coefficients = std::min(64u, 1u << (4 + (size_id << 1)));
        if (size_id > 1) (void)r.se();
        for (unsigned i = 0; i < coefficients; ++i) (void)r.se();
      }
      if (!r.ok()) return fail(Status::Decode, "the HEVC scaling list is truncated");
    }
  }
  return {};
}

/// st_ref_pic_set() of an SPS; returns NumDeltaPocs of set `index`.
[[nodiscard]] Result<std::uint32_t> parse_st_ref_pic_set(BitReader& r, std::uint32_t index,
                                                         std::span<const std::uint32_t> previous) {
  bool predicted = false;
  if (index != 0) predicted = r.flag();
  if (predicted) {
    // In an SPS the reference set is always the previous one (delta_idx = 1).
    (void)r.flag();  // delta_rps_sign
    if (r.ue() > 32767) return fail(Status::Decode, "invalid HEVC reference picture set");
    const std::uint32_t reference = previous[index - 1];
    std::uint32_t count = 0;
    for (std::uint32_t j = 0; j <= reference; ++j) {
      const bool used = r.flag();
      const bool use_delta = used ? true : r.flag();
      if (use_delta) ++count;
    }
    if (!r.ok() || count > 32) return fail(Status::Decode, "invalid HEVC reference picture set");
    return count;
  }
  const std::uint32_t negative = r.ue();
  const std::uint32_t positive = r.ue();
  if (negative > 16 || positive > 16 || negative + positive > 16) {
    return fail(Status::Decode, "invalid HEVC reference picture set");
  }
  for (std::uint32_t i = 0; i < negative + positive; ++i) {
    if (r.ue() > 32767) return fail(Status::Decode, "invalid HEVC reference picture set");
    (void)r.flag();
  }
  if (!r.ok()) return fail(Status::Decode, "the HEVC reference picture set is truncated");
  return negative + positive;
}

void skip_sub_layer_hrd(BitReader& r, std::uint32_t cpb_count, bool sub_pic) {
  for (std::uint32_t i = 0; i <= cpb_count && r.ok(); ++i) {
    (void)r.ue();
    (void)r.ue();
    if (sub_pic) {
      (void)r.ue();
      (void)r.ue();
    }
    (void)r.flag();
  }
}

[[nodiscard]] Result<void> skip_hrd_parameters(BitReader& r, unsigned max_sub_layers_minus1) {
  bool nal = false;
  bool vcl = false;
  bool sub_pic = false;
  nal = r.flag();
  vcl = r.flag();
  if (nal || vcl) {
    sub_pic = r.flag();
    if (sub_pic) r.skip(8 + 5 + 1 + 5);
    r.skip(4 + 4);
    if (sub_pic) r.skip(4);
    r.skip(5 + 5 + 5);
  }
  for (unsigned i = 0; i <= max_sub_layers_minus1 && r.ok(); ++i) {
    const bool fixed_general = r.flag();
    bool fixed_within = true;
    if (!fixed_general) fixed_within = r.flag();
    bool low_delay = false;
    if (fixed_within) {
      (void)r.ue();
    } else {
      low_delay = r.flag();
    }
    std::uint32_t cpb_count = 0;
    if (!low_delay) cpb_count = r.ue();
    if (cpb_count > 31) return fail(Status::Decode, "invalid HEVC HRD parameters");
    if (nal) skip_sub_layer_hrd(r, cpb_count, sub_pic);
    if (vcl) skip_sub_layer_hrd(r, cpb_count, sub_pic);
  }
  if (!r.ok()) return fail(Status::Decode, "the HEVC HRD parameters are truncated");
  return {};
}

/// vui_parameters(). Fields after an unparseable part keep their defaults:
/// the colour description comes first and is what matters most.
void parse_vui(BitReader& r, SequenceInfo& sps) {
  sps.vui_present = true;
  if (r.flag()) {  // aspect_ratio_info_present_flag
    const std::uint32_t idc = r.bits(8);
    static constexpr std::array<std::array<std::uint16_t, 2>, 17> table = {{{0, 0},
                                                                            {1, 1},
                                                                            {12, 11},
                                                                            {10, 11},
                                                                            {16, 11},
                                                                            {40, 33},
                                                                            {24, 11},
                                                                            {20, 11},
                                                                            {32, 11},
                                                                            {80, 33},
                                                                            {18, 11},
                                                                            {15, 11},
                                                                            {64, 33},
                                                                            {160, 99},
                                                                            {4, 3},
                                                                            {3, 2},
                                                                            {2, 1}}};
    if (idc == 255) {
      sps.sar_width = r.bits(16);
      sps.sar_height = r.bits(16);
    } else if (idc < table.size()) {
      sps.sar_width = table[idc][0];
      sps.sar_height = table[idc][1];
    }
  }
  if (r.flag()) (void)r.flag();  // overscan
  if (r.flag()) {                // video_signal_type_present_flag
    r.skip(3);
    sps.video_full_range = r.flag();
    if (r.flag()) {
      sps.colour_primaries = static_cast<std::uint8_t>(r.bits(8));
      sps.transfer_characteristics = static_cast<std::uint8_t>(r.bits(8));
      sps.matrix_coefficients = static_cast<std::uint8_t>(r.bits(8));
    }
  }
  if (r.flag()) {  // chroma_loc_info_present_flag
    sps.chroma_loc_present = true;
    sps.chroma_sample_loc_type = r.ue();
    (void)r.ue();
    if (sps.chroma_sample_loc_type > 5) sps.chroma_loc_present = false;
  }
  r.skip(3);       // neutral_chroma_indication, field_seq, frame_field_info_present
  if (r.flag()) {  // default_display_window_flag
    for (int i = 0; i < 4; ++i) (void)r.ue();
  }
  if (!r.ok()) return;
  if (r.flag()) {  // vui_timing_info_present_flag
    sps.num_units_in_tick = r.bits(32);
    sps.time_scale = r.bits(32);
    if (r.flag()) (void)r.ue();  // poc_proportional_to_timing
    if (r.flag()) {              // vui_hrd_parameters_present_flag
      if (!skip_hrd_parameters(r, sps.max_sub_layers - 1u)) return;
    }
  }
  if (!r.ok()) return;
  if (r.flag()) {  // bitstream_restriction_flag
    r.skip(3);
    const std::uint32_t segmentation = r.ue();
    if (r.ok() && segmentation < 4096) sps.min_spatial_segmentation = segmentation;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// NAL units
// ---------------------------------------------------------------------------

bool starts_with_start_code(ByteView data) noexcept {
  if (data.size() >= 3 && byte_at(data, 0) == 0 && byte_at(data, 1) == 0 && byte_at(data, 2) == 1) return true;
  return data.size() >= 4 && byte_at(data, 0) == 0 && byte_at(data, 1) == 0 && byte_at(data, 2) == 0 &&
         byte_at(data, 3) == 1;
}

std::vector<ByteView> split_annexb(ByteView data) {
  std::vector<ByteView> nals;
  std::size_t start = find_start_code(data, 0);
  while (start < data.size()) {
    const std::size_t payload = start + 3;
    const std::size_t next = find_start_code(data, payload);
    std::size_t end = next;
    // Trailing zero bytes belong to the next start code (or are padding).
    while (end > payload && byte_at(data, end - 1) == 0) --end;
    if (end > payload) nals.push_back(data.subspan(payload, end - payload));
    start = next;
  }
  return nals;
}

Result<std::vector<ByteView>> split_length_prefixed(ByteView data, unsigned length_size) {
  if (length_size != 1 && length_size != 2 && length_size != 4) {
    return fail(Status::Decode, "invalid NAL length size {}", length_size);
  }
  std::vector<ByteView> nals;
  std::size_t at = 0;
  while (at < data.size()) {
    if (data.size() - at < length_size) return fail(Status::Decode, "a NAL unit length is truncated");
    std::uint32_t length = 0;
    for (unsigned i = 0; i < length_size; ++i) length = (length << 8) | byte_at(data, at + i);
    at += length_size;
    if (length > data.size() - at) return fail(Status::Decode, "a NAL unit runs past the end of its packet");
    if (length > 0) nals.push_back(data.subspan(at, length));
    at += length;
  }
  return nals;
}

Bytes join_annexb(std::span<const ByteView> nals) {
  std::size_t total = 0;
  for (const ByteView nal : nals) total += nal.size() + 4;
  Bytes out;
  out.reserve(total);
  static constexpr std::array<std::byte, 4> start_code = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
  for (const ByteView nal : nals) {
    out.insert(out.end(), start_code.begin(), start_code.end());
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

Result<Bytes> join_length_prefixed(std::span<const ByteView> nals, unsigned length_size) {
  if (length_size != 1 && length_size != 2 && length_size != 4) {
    return fail(Status::InvalidArgument, "invalid NAL length size {}", length_size);
  }
  const std::uint64_t limit = length_size == 4 ? 0xFFFFFFFFull : (std::uint64_t{1} << (8 * length_size)) - 1;
  std::size_t total = 0;
  for (const ByteView nal : nals) {
    if (nal.size() > limit) return fail(Status::InvalidArgument, "a NAL unit is too large for {}-byte lengths", length_size);
    total += nal.size() + length_size;
  }
  Bytes out;
  out.reserve(total);
  for (const ByteView nal : nals) {
    for (unsigned i = length_size; i-- > 0;) {
      out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(nal.size()) >> (8 * i)) & 0xFF));
    }
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

Result<Bytes> annexb_to_length_prefixed(ByteView annexb, unsigned length_size) {
  const std::vector<ByteView> nals = split_annexb(annexb);
  return join_length_prefixed(nals, length_size);
}

Result<Bytes> length_prefixed_to_annexb(ByteView data, unsigned length_size) {
  CVT_TRY_ASSIGN(const std::vector<ByteView> nals, split_length_prefixed(data, length_size));
  return join_annexb(nals);
}

Bytes unescape_rbsp(ByteView nal) {
  Bytes out;
  out.reserve(nal.size());
  unsigned zeros = 0;
  for (std::size_t i = 0; i < nal.size(); ++i) {
    const std::uint8_t b = byte_at(nal, i);
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    out.push_back(nal[i]);
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Parameter sets
// ---------------------------------------------------------------------------

Result<SequenceInfo> parse_sps(ByteView sps_nal) {
  if (sps_nal.size() < 3 || nal_type(sps_nal) != static_cast<std::uint8_t>(NalType::Sps)) {
    return fail(Status::Decode, "not an HEVC sequence parameter set");
  }
  const Bytes rbsp = unescape_rbsp(sps_nal.subspan(2));
  BitReader r(rbsp);
  SequenceInfo sps;
  sps.vps_id = static_cast<std::uint8_t>(r.bits(4));
  const unsigned max_sub_layers_minus1 = r.bits(3);
  if (max_sub_layers_minus1 > 6) return fail(Status::Decode, "invalid HEVC SPS: too many sub-layers");
  sps.max_sub_layers = static_cast<std::uint8_t>(max_sub_layers_minus1 + 1);
  sps.temporal_id_nesting = r.flag();
  CVT_TRY(parse_profile_tier_level(r, max_sub_layers_minus1, sps.ptl));
  const std::uint32_t sps_id = r.ue();
  if (sps_id > 15) return fail(Status::Decode, "invalid HEVC SPS id {}", sps_id);
  sps.sps_id = static_cast<std::uint8_t>(sps_id);
  const std::uint32_t chroma = r.ue();
  if (chroma > 3) return fail(Status::Decode, "invalid HEVC chroma format {}", chroma);
  sps.chroma_format_idc = static_cast<std::uint8_t>(chroma);
  if (chroma == 3) sps.separate_colour_planes = r.flag();
  sps.coded_width = r.ue();
  sps.coded_height = r.ue();
  if (!r.ok() || sps.coded_width == 0 || sps.coded_height == 0 || sps.coded_width > max_dimension ||
      sps.coded_height > max_dimension) {
    return fail(Status::Decode, "invalid HEVC picture size {}x{}", sps.coded_width, sps.coded_height);
  }
  if (r.flag()) {  // conformance_window_flag
    const std::uint32_t unit_x = (chroma == 1 || chroma == 2) ? 2 : 1;
    const std::uint32_t unit_y = chroma == 1 ? 2 : 1;
    const std::uint64_t left = std::uint64_t{r.ue()} * unit_x;
    const std::uint64_t right = std::uint64_t{r.ue()} * unit_x;
    const std::uint64_t top = std::uint64_t{r.ue()} * unit_y;
    const std::uint64_t bottom = std::uint64_t{r.ue()} * unit_y;
    if (!r.ok() || left + right >= sps.coded_width || top + bottom >= sps.coded_height) {
      return fail(Status::Decode, "invalid HEVC conformance window");
    }
    sps.crop_left = static_cast<std::uint32_t>(left);
    sps.crop_right = static_cast<std::uint32_t>(right);
    sps.crop_top = static_cast<std::uint32_t>(top);
    sps.crop_bottom = static_cast<std::uint32_t>(bottom);
  }
  sps.width = sps.coded_width - sps.crop_left - sps.crop_right;
  sps.height = sps.coded_height - sps.crop_top - sps.crop_bottom;
  const std::uint32_t depth_luma = r.ue();
  const std::uint32_t depth_chroma = r.ue();
  if (depth_luma > 8 || depth_chroma > 8) return fail(Status::Decode, "invalid HEVC bit depth");
  sps.bit_depth_luma = static_cast<std::uint8_t>(depth_luma + 8);
  sps.bit_depth_chroma = static_cast<std::uint8_t>(depth_chroma + 8);
  const std::uint32_t poc_bits = r.ue();
  if (poc_bits > 12) return fail(Status::Decode, "invalid HEVC SPS: picture order count size");
  sps.log2_max_poc_lsb = static_cast<std::uint8_t>(poc_bits + 4);
  const bool ordering_per_layer = r.flag();
  for (unsigned i = ordering_per_layer ? 0 : max_sub_layers_minus1; i <= max_sub_layers_minus1; ++i) {
    sps.max_dec_pic_buffering = r.ue() + 1;
    sps.max_num_reorder_pics = r.ue();
    (void)r.ue();
  }
  if (!r.ok() || sps.max_dec_pic_buffering > 16 || sps.max_num_reorder_pics > 16) {
    return fail(Status::Decode, "invalid HEVC SPS: picture buffer sizes");
  }
  const std::uint32_t log2_min_cb = r.ue() + 3;
  const std::uint32_t log2_ctb = log2_min_cb + r.ue();
  if (log2_min_cb > 6 || log2_ctb < 4 || log2_ctb > 6) return fail(Status::Decode, "invalid HEVC coding block sizes");
  sps.log2_min_cb_size = static_cast<std::uint8_t>(log2_min_cb);
  sps.log2_ctb_size = static_cast<std::uint8_t>(log2_ctb);
  (void)r.ue();  // log2_min_luma_transform_block_size_minus2
  (void)r.ue();  // log2_diff_max_min_luma_transform_block_size
  (void)r.ue();  // max_transform_hierarchy_depth_inter
  (void)r.ue();  // max_transform_hierarchy_depth_intra
  sps.scaling_list_enabled = r.flag();
  if (sps.scaling_list_enabled && r.flag()) CVT_TRY(skip_scaling_list_data(r));
  (void)r.flag();  // amp_enabled_flag
  (void)r.flag();  // sample_adaptive_offset_enabled_flag
  if (r.flag()) {  // pcm_enabled_flag
    r.skip(4 + 4);
    (void)r.ue();
    (void)r.ue();
    (void)r.flag();
  }
  const std::uint32_t sets = r.ue();
  if (sets > 64) return fail(Status::Decode, "invalid HEVC SPS: {} reference picture sets", sets);
  sps.num_short_term_ref_pic_sets = sets;
  std::vector<std::uint32_t> delta_pocs(sets, 0);
  for (std::uint32_t i = 0; i < sets; ++i) {
    CVT_TRY_ASSIGN(delta_pocs[i], parse_st_ref_pic_set(r, i, delta_pocs));
  }
  sps.long_term_refs_present = r.flag();
  if (sps.long_term_refs_present) {
    const std::uint32_t count = r.ue();
    if (count > 32) return fail(Status::Decode, "invalid HEVC SPS: long-term reference pictures");
    for (std::uint32_t i = 0; i < count; ++i) {
      r.skip(sps.log2_max_poc_lsb);
      (void)r.flag();
    }
  }
  (void)r.flag();  // sps_temporal_mvp_enabled_flag
  (void)r.flag();  // strong_intra_smoothing_enabled_flag
  if (!r.ok()) return fail(Status::Decode, "the HEVC SPS is truncated");
  if (r.flag()) parse_vui(r, sps);  // vui_parameters_present_flag; best effort
  return sps;
}

Result<PictureParamsLite> parse_pps_lite(ByteView pps_nal) {
  if (pps_nal.size() < 3 || nal_type(pps_nal) != static_cast<std::uint8_t>(NalType::Pps)) {
    return fail(Status::Decode, "not an HEVC picture parameter set");
  }
  const Bytes rbsp = unescape_rbsp(pps_nal.subspan(2));
  BitReader r(rbsp);
  PictureParamsLite pps;
  const std::uint32_t pps_id = r.ue();
  const std::uint32_t sps_id = r.ue();
  if (pps_id > 63 || sps_id > 15) return fail(Status::Decode, "invalid HEVC PPS ids");
  pps.pps_id = static_cast<std::uint8_t>(pps_id);
  pps.sps_id = static_cast<std::uint8_t>(sps_id);
  pps.dependent_slice_segments_enabled = r.flag();
  pps.output_flag_present = r.flag();
  pps.num_extra_slice_header_bits = static_cast<std::uint8_t>(r.bits(3));
  (void)r.flag();  // sign_data_hiding_enabled_flag
  (void)r.flag();  // cabac_init_present_flag
  (void)r.ue();
  (void)r.ue();
  (void)r.se();    // init_qp_minus26
  (void)r.flag();  // constrained_intra_pred_flag
  (void)r.flag();  // transform_skip_enabled_flag
  if (r.flag()) (void)r.ue();  // cu_qp_delta_enabled_flag, diff_cu_qp_delta_depth
  (void)r.se();
  (void)r.se();
  r.skip(4);  // slice_chroma_qp_offsets, weighted_pred, weighted_bipred, transquant_bypass
  pps.tiles_enabled = r.flag();
  pps.entropy_coding_sync_enabled = r.flag();
  if (!r.ok()) return fail(Status::Decode, "the HEVC PPS is truncated");
  return pps;
}

ir::PixelFormat pixel_format(const SequenceInfo& sps) noexcept {
  const bool wide = sps.bit_depth_luma > 8 || sps.bit_depth_chroma > 8;
  switch (sps.chroma_format_idc) {
    case 2: return wide ? ir::PixelFormat::I422P10 : ir::PixelFormat::I422;
    case 3: return wide ? ir::PixelFormat::I444P10 : ir::PixelFormat::I444;
    default: return wide ? ir::PixelFormat::I420P10 : ir::PixelFormat::I420;
  }
}

ir::ColorInfo color_info(const SequenceInfo& sps) noexcept {
  ir::ColorInfo color;
  switch (sps.matrix_coefficients) {
    case 1: color.matrix = ir::ColorMatrix::BT709; break;
    case 5:
    case 6: color.matrix = ir::ColorMatrix::BT601; break;
    case 9:
    case 10: color.matrix = ir::ColorMatrix::BT2020; break;
    default: color.matrix = sps.height < 720 ? ir::ColorMatrix::BT601 : ir::ColorMatrix::BT709; break;
  }
  color.range = sps.video_full_range ? ir::ColorRange::Full : ir::ColorRange::Limited;
  color.siting = ir::ChromaSiting::Left;
  if (sps.chroma_loc_present) {
    if (sps.chroma_sample_loc_type == 1) color.siting = ir::ChromaSiting::Center;
    if (sps.chroma_sample_loc_type == 2) color.siting = ir::ChromaSiting::TopLeft;
  }
  color.primaries = sps.colour_primaries;
  color.transfer = sps.transfer_characteristics;
  return color;
}

// ---------------------------------------------------------------------------
// hvcC
// ---------------------------------------------------------------------------

Result<DecoderConfig> parse_hvcc(ByteView hvcc) {
  util::ByteReader r(hvcc);
  DecoderConfig config;
  if (r.u8() != 1) return fail(Status::Decode, "unsupported hvcC version");
  const std::uint8_t b1 = r.u8();
  config.ptl.profile_space = static_cast<std::uint8_t>(b1 >> 6);
  config.ptl.tier = ((b1 >> 5) & 1) != 0;
  config.ptl.profile_idc = static_cast<std::uint8_t>(b1 & 0x1F);
  config.ptl.compatibility_flags = r.u32be();
  config.ptl.constraint_flags = (std::uint64_t{r.u16be()} << 32) | r.u32be();
  config.ptl.level_idc = r.u8();
  config.min_spatial_segmentation = static_cast<std::uint16_t>(r.u16be() & 0x0FFF);
  config.parallelism_type = static_cast<std::uint8_t>(r.u8() & 3);
  config.chroma_format_idc = static_cast<std::uint8_t>(r.u8() & 3);
  config.bit_depth_luma = static_cast<std::uint8_t>((r.u8() & 7) + 8);
  config.bit_depth_chroma = static_cast<std::uint8_t>((r.u8() & 7) + 8);
  config.avg_frame_rate = r.u16be();
  const std::uint8_t b21 = r.u8();
  config.constant_frame_rate = static_cast<std::uint8_t>(b21 >> 6);
  config.num_temporal_layers = static_cast<std::uint8_t>((b21 >> 3) & 7);
  config.temporal_id_nested = ((b21 >> 2) & 1) != 0;
  config.length_size = static_cast<std::uint8_t>((b21 & 3) + 1);
  if (config.length_size == 3) return fail(Status::Decode, "invalid hvcC NAL length size 3");
  const std::uint8_t arrays = r.u8();
  if (!r.ok()) return fail(Status::Decode, "the hvcC record is truncated");
  for (std::uint8_t a = 0; a < arrays; ++a) {
    (void)r.u8();  // completeness + type
    const std::uint16_t count = r.u16be();
    if (!r.ok()) return fail(Status::Decode, "the hvcC record is truncated");
    for (std::uint16_t n = 0; n < count; ++n) {
      const std::uint16_t length = r.u16be();
      const ByteView nal = r.bytes(length);
      if (!r.ok()) return fail(Status::Decode, "the hvcC record is truncated");
      if (!nal.empty()) config.nals.emplace_back(nal.begin(), nal.end());
    }
  }
  return config;
}

Result<Bytes> build_hvcc(std::span<const ByteView> parameter_sets, unsigned length_size) {
  if (length_size != 1 && length_size != 2 && length_size != 4) {
    return fail(Status::InvalidArgument, "invalid NAL length size {}", length_size);
  }
  std::array<std::vector<ByteView>, 4> arrays;  // VPS, SPS, PPS, prefix SEI
  static constexpr std::array<std::uint8_t, 4> types = {32, 33, 34, 39};
  std::optional<SequenceInfo> sps;
  for (const ByteView nal : parameter_sets) {
    const std::uint8_t type = nal_type(nal);
    for (std::size_t i = 0; i < types.size(); ++i) {
      if (type != types[i]) continue;
      if (nal.size() > 0xFFFF) return fail(Status::InvalidArgument, "a parameter set is too large for hvcC");
      arrays[i].push_back(nal);
      if (type == 33 && !sps) {
        CVT_TRY_ASSIGN(SequenceInfo parsed, parse_sps(nal));
        sps = parsed;
      }
    }
  }
  if (!sps) return fail(Status::InvalidArgument, "an hvcC record needs a sequence parameter set");
  bool wpp = false;
  bool tiles = false;
  for (const ByteView pps : arrays[2]) {
    if (const auto parsed = parse_pps_lite(pps)) {
      wpp = wpp || parsed->entropy_coding_sync_enabled;
      tiles = tiles || parsed->tiles_enabled;
    }
  }
  util::ByteWriter w;
  w.u8(1);
  w.u8(static_cast<std::uint8_t>((sps->ptl.profile_space << 6) | (sps->ptl.tier ? 0x20 : 0) |
                                 (sps->ptl.profile_idc & 0x1F)));
  w.u32be(sps->ptl.compatibility_flags);
  w.u16be(static_cast<std::uint16_t>(sps->ptl.constraint_flags >> 32));
  w.u32be(static_cast<std::uint32_t>(sps->ptl.constraint_flags & 0xFFFFFFFFu));
  w.u8(sps->ptl.level_idc);
  w.u16be(static_cast<std::uint16_t>(0xF000 | (sps->min_spatial_segmentation & 0x0FFF)));
  // parallelismType: 0 mixed/unknown, 2 tiles, 3 wavefront (slices are not inspected).
  const std::uint8_t parallelism = (wpp && !tiles) ? 3 : ((tiles && !wpp) ? 2 : 0);
  w.u8(static_cast<std::uint8_t>(0xFC | parallelism));
  w.u8(static_cast<std::uint8_t>(0xFC | sps->chroma_format_idc));
  w.u8(static_cast<std::uint8_t>(0xF8 | (sps->bit_depth_luma - 8)));
  w.u8(static_cast<std::uint8_t>(0xF8 | (sps->bit_depth_chroma - 8)));
  w.u16be(0);  // avgFrameRate unspecified
  w.u8(static_cast<std::uint8_t>((std::min<unsigned>(sps->max_sub_layers, 7) << 3) |
                                 (sps->temporal_id_nesting ? 4 : 0) | ((length_size - 1) & 3)));
  std::uint8_t count = 0;
  for (const auto& array : arrays) count = static_cast<std::uint8_t>(count + (array.empty() ? 0 : 1));
  w.u8(count);
  for (std::size_t i = 0; i < arrays.size(); ++i) {
    if (arrays[i].empty()) continue;
    // array_completeness = 1 for parameter sets (all of them are here).
    w.u8(static_cast<std::uint8_t>((i < 3 ? 0x80 : 0) | types[i]));
    w.u16be(static_cast<std::uint16_t>(arrays[i].size()));
    for (const ByteView nal : arrays[i]) {
      w.u16be(static_cast<std::uint16_t>(nal.size()));
      w.bytes(nal);
    }
  }
  return w.take();
}

// ---------------------------------------------------------------------------
// Access units and picture order counts
// ---------------------------------------------------------------------------

bool first_slice_in_picture(ByteView nal) noexcept {
  return nal.size() >= 3 && (std::to_integer<std::uint8_t>(nal[2]) & 0x80) != 0;
}

std::optional<std::vector<ByteView>> AccessUnitSplitter::push(ByteView nal) {
  const std::uint8_t type = nal_type(nal);
  bool boundary = false;
  if (has_vcl_) {
    if (is_vcl(type)) {
      boundary = first_slice_in_picture(nal);
    } else {
      boundary = starts_access_unit(type);
    }
  }
  std::optional<std::vector<ByteView>> done;
  if (boundary) {
    done = std::exchange(current_, {});
    has_vcl_ = false;
  }
  current_.push_back(nal);
  if (is_vcl(type)) has_vcl_ = true;
  // End of sequence / bitstream close the access unit they end.
  if ((type == 36 || type == 37) && !done) {
    done = std::exchange(current_, {});
    has_vcl_ = false;
  }
  return done;
}

std::optional<std::vector<ByteView>> AccessUnitSplitter::finish() {
  if (current_.empty()) return std::nullopt;
  has_vcl_ = false;
  return std::exchange(current_, {});
}

void PocTracker::parameter_set(ByteView nal) {
  const std::uint8_t type = nal_type(nal);
  if (type == 33) {
    if (auto sps = parse_sps(nal)) sps_[sps->sps_id] = *sps;
  } else if (type == 34) {
    if (auto pps = parse_pps_lite(nal)) pps_[pps->pps_id] = *pps;
  } else if (type == 36) {
    after_eos_ = true;
  }
}

std::optional<std::int64_t> PocTracker::picture(ByteView nal, bool& new_sequence) {
  new_sequence = false;
  const std::uint8_t type = nal_type(nal);
  if (!is_vcl(type) || nal.size() < 3) return std::nullopt;
  const unsigned temporal_id_plus1 = (std::to_integer<unsigned>(nal[1]) & 7u);
  const Bytes rbsp = unescape_rbsp(nal.subspan(2));
  BitReader r(rbsp);
  const bool first = r.flag();
  if (!first) return std::nullopt;
  if (is_irap(type)) (void)r.flag();  // no_output_of_prior_pics_flag
  const auto pps = pps_.find(r.ue());
  if (!r.ok() || pps == pps_.end()) return std::nullopt;
  const auto sps = sps_.find(pps->second.sps_id);
  if (sps == sps_.end()) return std::nullopt;
  r.skip(pps->second.num_extra_slice_header_bits);
  (void)r.ue();  // slice_type
  if (pps->second.output_flag_present) (void)r.flag();
  if (sps->second.separate_colour_planes) r.skip(2);
  std::int64_t lsb = 0;
  if (!is_idr(type)) lsb = r.bits(sps->second.log2_max_poc_lsb);
  if (!r.ok()) return std::nullopt;

  const std::int64_t max_lsb = std::int64_t{1} << sps->second.log2_max_poc_lsb;
  // IDR, BLA, and a CRA that begins the stream or follows an end of sequence.
  const bool no_rasl_output = is_idr(type) || (type >= 16 && type <= 18) || (type == 21 && (first_picture_ || after_eos_));
  std::int64_t msb = 0;
  if (is_irap(type) && no_rasl_output) {
    msb = 0;
    new_sequence = true;
  } else {
    const std::int64_t prev_lsb = ((prev_tid0_poc_ % max_lsb) + max_lsb) % max_lsb;
    const std::int64_t prev_msb = prev_tid0_poc_ - prev_lsb;
    if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) {
      msb = prev_msb + max_lsb;
    } else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) {
      msb = prev_msb - max_lsb;
    } else {
      msb = prev_msb;
    }
  }
  const std::int64_t poc = msb + lsb;
  // prevTid0Pic: TemporalId 0 and not RASL, RADL or a sub-layer non-reference picture.
  const bool sub_layer_non_reference = type <= 14 && (type % 2) == 0;
  const bool leading = type >= 6 && type <= 9;
  if (temporal_id_plus1 == 1 && !leading && !sub_layer_non_reference) prev_tid0_poc_ = poc;
  first_picture_ = false;
  after_eos_ = false;
  return poc;
}

}  // namespace cvt::hevc
