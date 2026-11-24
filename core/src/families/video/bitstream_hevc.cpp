// Convertible core - video family: HEVC sequence/picture parameter sets and
// picture order counts (ITU-T H.265 7.3.2, 8.3.1), for raw streams and
// containers that carry no presentation times.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "families/video/bitstream.hpp"

namespace cvt::video::bits {

namespace {

/// The SPS fields needed for sizes, timing and slice headers.
struct HevcSps {
  SpsInfo info;
  std::uint32_t sps_id = 0;
  std::uint32_t max_sub_layers = 1;
  bool separate_colour_planes = false;
  int log2_max_poc_lsb = 4;
};

struct HevcPps {
  std::uint32_t sps_id = 0;
  bool output_flag_present = false;
  int num_extra_slice_header_bits = 0;
};

bool skip_profile_tier_level(BitReader& r, std::uint32_t max_sub_layers_minus1, SpsInfo& info) {
  r.skip(2 + 1);
  info.profile = static_cast<int>(r.bits(5));
  r.skip(32 + 48);
  info.level = static_cast<int>(r.bits(8));
  std::array<bool, 8> profile_present{};
  std::array<bool, 8> level_present{};
  for (std::uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
    profile_present[i] = r.flag();
    level_present[i] = r.flag();
  }
  if (max_sub_layers_minus1 > 0) {
    for (std::uint32_t i = max_sub_layers_minus1; i < 8; ++i) r.skip(2);
  }
  for (std::uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
    if (profile_present[i]) r.skip(88);
    if (level_present[i]) r.skip(8);
  }
  return !r.failed();
}

bool skip_scaling_list_data(BitReader& r) {
  for (std::uint32_t size_id = 0; size_id < 4; ++size_id) {
    for (std::uint32_t matrix_id = 0; matrix_id < 6; matrix_id += (size_id == 3) ? 3 : 1) {
      if (!r.flag()) {
        if (r.ue() > matrix_id) return false;
      } else {
        const std::uint32_t coefficients = std::min(64u, 1u << (4 + (size_id << 1)));
        if (size_id > 1) (void)r.se();
        for (std::uint32_t i = 0; i < coefficients; ++i) (void)r.se();
      }
      if (r.failed()) return false;
    }
  }
  return true;
}

/// st_ref_pic_set() of an SPS; NumDeltaPocs of set `index`, or nullopt.
std::optional<std::uint32_t> skip_st_ref_pic_set(BitReader& r, std::uint32_t index, const std::vector<std::uint32_t>& previous) {
  const bool predicted = index != 0 && r.flag();
  if (predicted) {
    r.skip(1);  // delta_rps_sign; in an SPS delta_idx is always 1
    if (r.ue() > 32767) return std::nullopt;
    const std::uint32_t reference = previous[index - 1];
    std::uint32_t count = 0;
    for (std::uint32_t j = 0; j <= reference; ++j) {
      const bool used = r.flag();
      if (used || r.flag()) ++count;
    }
    if (r.failed() || count > 32) return std::nullopt;
    return count;
  }
  const std::uint32_t negative = r.ue();
  const std::uint32_t positive = r.ue();
  if (negative > 16 || positive > 16 || negative + positive > 16) return std::nullopt;
  for (std::uint32_t i = 0; i < negative + positive; ++i) {
    if (r.ue() > 32767) return std::nullopt;
    r.skip(1);
  }
  if (r.failed()) return std::nullopt;
  return negative + positive;
}

void skip_sub_layer_hrd(BitReader& r, std::uint32_t cpb_count, bool sub_pic) {
  for (std::uint32_t i = 0; i <= cpb_count && !r.failed(); ++i) {
    r.ue();
    r.ue();
    if (sub_pic) {
      r.ue();
      r.ue();
    }
    r.skip(1);
  }
}

bool skip_hrd_parameters(BitReader& r, std::uint32_t max_sub_layers_minus1) {
  const bool nal = r.flag();
  const bool vcl = r.flag();
  bool sub_pic = false;
  if (nal || vcl) {
    sub_pic = r.flag();
    if (sub_pic) r.skip(8 + 5 + 1 + 5);
    r.skip(4 + 4);
    if (sub_pic) r.skip(4);
    r.skip(5 + 5 + 5);
  }
  for (std::uint32_t i = 0; i <= max_sub_layers_minus1 && !r.failed(); ++i) {
    const bool fixed_general = r.flag();
    const bool fixed_within = fixed_general || r.flag();
    bool low_delay = false;
    if (fixed_within) {
      r.ue();
    } else {
      low_delay = r.flag();
    }
    const std::uint32_t cpb_count = low_delay ? 0 : r.ue();
    if (cpb_count > 31) return false;
    if (nal) skip_sub_layer_hrd(r, cpb_count, sub_pic);
    if (vcl) skip_sub_layer_hrd(r, cpb_count, sub_pic);
  }
  return !r.failed();
}

/// vui_parameters(): sample aspect ratio and timing; best effort.
void parse_vui(BitReader& r, std::uint32_t max_sub_layers_minus1, SpsInfo& info) {
  if (r.flag()) {  // aspect_ratio_info_present_flag
    const std::uint32_t idc = r.bits(8);
    static constexpr std::array<std::array<std::uint16_t, 2>, 17> table = {
        {{0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11},
         {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1}}};
    if (idc == 255) {
      info.sar_num = r.bits(16);
      info.sar_den = r.bits(16);
    } else if (idc < table.size()) {
      info.sar_num = table[idc][0];
      info.sar_den = table[idc][1];
    }
  }
  if (r.flag()) r.skip(1);  // overscan
  if (r.flag()) {           // video_signal_type_present_flag
    r.skip(4);
    if (r.flag()) r.skip(24);
  }
  if (r.flag()) {  // chroma_loc_info_present_flag
    r.ue();
    r.ue();
  }
  r.skip(3);       // neutral_chroma_indication, field_seq, frame_field_info_present
  if (r.flag()) {  // default_display_window_flag
    for (int i = 0; i < 4; ++i) r.ue();
  }
  if (r.failed()) return;
  if (r.flag()) {  // vui_timing_info_present_flag
    const std::uint32_t units = r.bits(32);
    const std::uint32_t scale = r.bits(32);
    if (!r.failed() && units > 0 && scale > 0) {
      info.fps_num = scale;
      info.fps_den = units;
    }
    if (r.flag()) r.ue();  // poc_proportional_to_timing
    if (r.flag()) (void)skip_hrd_parameters(r, max_sub_layers_minus1);
  }
}

std::optional<HevcSps> parse_sps(ByteView nal) {
  if (nal.size() < 3 || hevc_nal_type(nal) != 33) return std::nullopt;
  const Bytes rbsp = unescape_rbsp(nal.subspan(2));
  BitReader r(rbsp);
  HevcSps sps;
  SpsInfo& info = sps.info;
  r.skip(4);  // sps_video_parameter_set_id
  const std::uint32_t max_sub_layers_minus1 = r.bits(3);
  if (max_sub_layers_minus1 > 6) return std::nullopt;
  sps.max_sub_layers = max_sub_layers_minus1 + 1;
  r.skip(1);  // temporal_id_nesting
  if (!skip_profile_tier_level(r, max_sub_layers_minus1, info)) return std::nullopt;
  sps.sps_id = r.ue();
  if (sps.sps_id > 15) return std::nullopt;
  const std::uint32_t chroma = r.ue();
  if (chroma > 3) return std::nullopt;
  info.chroma_format = static_cast<int>(chroma);
  if (chroma == 3) sps.separate_colour_planes = r.flag();
  const std::uint32_t width = r.ue();
  const std::uint32_t height = r.ue();
  std::array<std::uint32_t, 4> window{};
  if (r.flag()) {
    for (std::uint32_t& w : window) w = r.ue();
  }
  const std::uint32_t depth_luma = r.ue();
  const std::uint32_t depth_chroma = r.ue();
  if (r.failed() || width == 0 || height == 0 || width > 65536 || height > 65536 || depth_luma > 8 || depth_chroma > 8) {
    return std::nullopt;
  }
  info.bit_depth_luma = static_cast<int>(depth_luma) + 8;
  info.bit_depth_chroma = static_cast<int>(depth_chroma) + 8;
  const std::uint64_t sub_w = chroma == 1 || chroma == 2 ? 2 : 1;
  const std::uint64_t sub_h = chroma == 1 ? 2 : 1;
  const std::uint64_t cx = sub_w * (std::uint64_t{window[0]} + window[1]);
  const std::uint64_t cy = sub_h * (std::uint64_t{window[2]} + window[3]);
  if (cx >= width || cy >= height) return std::nullopt;
  info.width = static_cast<std::uint32_t>(width - cx);
  info.height = static_cast<std::uint32_t>(height - cy);

  // Everything above is what sizes need; the rest is best effort (timing, reordering).
  const std::uint32_t poc_bits = r.ue();
  if (poc_bits > 12) return std::nullopt;
  sps.log2_max_poc_lsb = static_cast<int>(poc_bits) + 4;
  const bool per_layer = r.flag();
  for (std::uint32_t i = per_layer ? 0 : max_sub_layers_minus1; i <= max_sub_layers_minus1; ++i) {
    r.ue();
    const std::uint32_t reorder = r.ue();
    r.ue();
    if (!r.failed() && reorder <= 16) info.max_num_reorder = reorder;
  }
  r.ue();  // log2_min_luma_coding_block_size_minus3
  r.ue();  // log2_diff_max_min_luma_coding_block_size
  r.ue();
  r.ue();
  r.ue();
  r.ue();
  if (r.flag() && r.flag() && !skip_scaling_list_data(r)) return sps;  // scaling_list_enabled + data
  r.skip(2);                                                            // amp, sample_adaptive_offset
  if (r.flag()) {                                                       // pcm_enabled_flag
    r.skip(8);
    r.ue();
    r.ue();
    r.skip(1);
  }
  const std::uint32_t sets = r.ue();
  if (r.failed() || sets > 64) return sps;
  std::vector<std::uint32_t> deltas(sets, 0);
  for (std::uint32_t i = 0; i < sets; ++i) {
    const auto count = skip_st_ref_pic_set(r, i, deltas);
    if (!count) return sps;
    deltas[i] = *count;
  }
  if (r.flag()) {  // long_term_ref_pics_present_flag
    const std::uint32_t count = r.ue();
    if (count > 32) return sps;
    for (std::uint32_t i = 0; i < count; ++i) r.skip(static_cast<std::uint64_t>(sps.log2_max_poc_lsb) + 1);
  }
  r.skip(2);  // temporal_mvp, strong_intra_smoothing
  if (!r.failed() && r.flag()) parse_vui(r, max_sub_layers_minus1, info);
  return sps;
}

std::optional<std::pair<std::uint32_t, HevcPps>> parse_pps(ByteView nal) {
  if (nal.size() < 3) return std::nullopt;
  const Bytes rbsp = unescape_rbsp(nal.subspan(2));
  BitReader r(rbsp);
  const std::uint32_t id = r.ue();
  HevcPps pps;
  pps.sps_id = r.ue();
  r.skip(1);  // dependent_slice_segments_enabled_flag
  pps.output_flag_present = r.flag();
  pps.num_extra_slice_header_bits = static_cast<int>(r.bits(3));
  if (r.failed() || id > 63 || pps.sps_id > 15) return std::nullopt;
  return std::pair{id, pps};
}

constexpr bool is_vcl(int type) noexcept { return type >= 0 && type <= 31; }
constexpr bool is_irap(int type) noexcept { return type >= 16 && type <= 23; }
constexpr bool is_idr(int type) noexcept { return type == 19 || type == 20; }

}  // namespace

std::optional<SpsInfo> parse_hevc_sps(ByteView nal) {
  auto sps = parse_sps(nal);
  if (!sps) return std::nullopt;
  return sps->info;
}

struct HevcPocTracker::State {
  std::map<std::uint32_t, HevcSps> sps;
  std::map<std::uint32_t, HevcPps> pps;
  std::int64_t prev_tid0_poc = 0;
  bool first_picture = true;
  bool after_eos = false;
};

HevcPocTracker::HevcPocTracker() : state_(std::make_unique<State>()) {}
HevcPocTracker::~HevcPocTracker() = default;

std::optional<std::pair<std::int64_t, bool>> HevcPocTracker::next(const std::vector<ByteView>& access_unit) {
  State& s = *state_;
  for (const ByteView nal : access_unit) {
    const int type = hevc_nal_type(nal);
    if (type == 33) {
      if (auto sps = parse_sps(nal)) s.sps[sps->sps_id] = *sps;
    } else if (type == 34) {
      if (auto pps = parse_pps(nal)) s.pps[pps->first] = pps->second;
    } else if (type == 36) {
      s.after_eos = true;
    }
  }
  for (const ByteView nal : access_unit) {
    const int type = hevc_nal_type(nal);
    if (!is_vcl(type) || nal.size() < 3) continue;
    const unsigned temporal_id_plus1 = std::to_integer<unsigned>(nal[1]) & 7u;
    const Bytes rbsp = unescape_rbsp(nal.subspan(2, std::min<std::size_t>(nal.size() - 2, 64)));
    BitReader r(rbsp);
    if (!r.flag()) continue;  // not the first slice segment of the picture
    if (is_irap(type)) r.skip(1);  // no_output_of_prior_pics_flag
    const auto pps = s.pps.find(r.ue());
    if (r.failed() || pps == s.pps.end()) return std::nullopt;
    const auto sps = s.sps.find(pps->second.sps_id);
    if (sps == s.sps.end()) return std::nullopt;
    r.skip(static_cast<std::uint64_t>(pps->second.num_extra_slice_header_bits));
    r.ue();  // slice_type
    if (pps->second.output_flag_present) r.skip(1);
    if (sps->second.separate_colour_planes) r.skip(2);
    std::int64_t lsb = 0;
    if (!is_idr(type)) lsb = r.bits(sps->second.log2_max_poc_lsb);
    if (r.failed()) return std::nullopt;

    const std::int64_t max_lsb = std::int64_t{1} << sps->second.log2_max_poc_lsb;
    // IDR, BLA, and a CRA that begins the stream or follows an end of sequence.
    const bool no_rasl_output = is_idr(type) || (type >= 16 && type <= 18) || (type == 21 && (s.first_picture || s.after_eos));
    std::int64_t msb = 0;
    bool new_sequence = false;
    if (is_irap(type) && no_rasl_output) {
      new_sequence = true;
    } else {
      const std::int64_t prev_lsb = ((s.prev_tid0_poc % max_lsb) + max_lsb) % max_lsb;
      const std::int64_t prev_msb = s.prev_tid0_poc - prev_lsb;
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
    if (temporal_id_plus1 == 1 && !leading && !sub_layer_non_reference) s.prev_tid0_poc = poc;
    s.first_picture = false;
    s.after_eos = false;
    return std::pair{poc, new_sequence};
  }
  return std::nullopt;
}

}  // namespace cvt::video::bits
