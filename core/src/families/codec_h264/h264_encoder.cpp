// Convertible core - codec_h264 family: H.264 encoder (libavc wrapper).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <numeric>
#include <utility>

#include "util/bytes.hpp"
#include "codec/colorspace.hpp"
#include "families/codec_h264/h264_bitstream.hpp"
#include "families/codec_h264/h264_codec.hpp"

// clang-format off
#include "ih264_typedefs.h"
#include "iv2.h"
#include "ive2.h"
#include "ih264e.h"
// clang-format on

namespace cvt::h264 {

namespace {

/// Level limits (ITU-T H.264 table A-1): level_idc, max macroblocks per
/// second, max frame size in macroblocks.
struct LevelLimit {
  std::uint32_t level;
  std::uint64_t mbs_per_second;
  std::uint64_t frame_mbs;
};
constexpr std::array<LevelLimit, 16> level_limits = {{{10, 1485, 99},
                                                      {11, 3000, 396},
                                                      {12, 6000, 396},
                                                      {13, 11880, 396},
                                                      {20, 11880, 396},
                                                      {21, 19800, 792},
                                                      {22, 20250, 1620},
                                                      {30, 40500, 1620},
                                                      {31, 108000, 3600},
                                                      {32, 216000, 5120},
                                                      {40, 245760, 8192},
                                                      {41, 245760, 8192},
                                                      {42, 522240, 8704},
                                                      {50, 589824, 22080},
                                                      {51, 983040, 36864},
                                                      {52, 2073600, 36864}}};

/// The smallest level that fits the picture size and frame rate; never below
/// 3.0, which every decoder handles and which leaves room for the bit rate of
/// small, high-quality videos.
[[nodiscard]] std::uint32_t pick_level(std::uint32_t width, std::uint32_t height, double fps) {
  const std::uint64_t mbs = ((std::uint64_t{width} + 15) / 16) * ((std::uint64_t{height} + 15) / 16);
  const double rate = fps > 0 ? fps : 30.0;
  for (const LevelLimit& limit : level_limits) {
    if (limit.level < 30 || limit.level > 51) continue;
    if (mbs <= limit.frame_mbs && static_cast<double>(mbs) * rate <= static_cast<double>(limit.mbs_per_second)) {
      return limit.level;
    }
  }
  return 51;  // the highest level libavc's encoder knows
}

[[nodiscard]] std::int64_t ms_to_ticks(std::int64_t ms, std::uint32_t timescale) noexcept {
  const std::int64_t scale = timescale == 0 ? 1000 : timescale;
  std::int64_t whole = ms / 1000;
  std::int64_t rest = ms % 1000;
  if (rest < 0) {
    rest += 1000;
    --whole;
  }
  return whole * scale + (rest * scale + 500) / 1000;
}

/// ITU-T H.273 codes for a ColorInfo: colour primaries, transfer, matrix.
struct ColourCodes {
  std::uint8_t primaries;
  std::uint8_t transfer;
  std::uint8_t matrix;
};
[[nodiscard]] ColourCodes colour_codes(const ir::ColorInfo& color) noexcept {
  switch (color.matrix) {
    case ir::ColorMatrix::BT601: return {6, 6, 6};    // SMPTE 170M
    case ir::ColorMatrix::BT2020: return {9, 14, 9};  // BT.2020 (10-bit transfer = BT.709 curve)
    case ir::ColorMatrix::BT709:
    default: return {1, 1, 1};
  }
}

template <class In, class Out>
void prepare(In& in, Out& out, IVE_CONTROL_API_COMMAND_TYPE_T command) {
  in.u4_size = sizeof(In);
  out.u4_size = sizeof(Out);
  in.e_cmd = IVE_CMD_VIDEO_CTL;
  in.e_sub_cmd = command;
  in.u4_timestamp_high = 0xFFFFFFFFu;
  in.u4_timestamp_low = 0xFFFFFFFFu;
}

}  // namespace

int quality_to_qp(int quality) noexcept {
  // Piecewise linear through (1, 46), (50, 33), (75, 27), (90, 21), (100, 10).
  static constexpr std::array<std::pair<int, int>, 5> anchors = {{{1, 46}, {50, 33}, {75, 27}, {90, 21}, {100, 10}}};
  const int q = std::clamp(quality, 1, 100);
  for (std::size_t i = 1; i < anchors.size(); ++i) {
    const auto [q0, qp0] = anchors[i - 1];
    const auto [q1, qp1] = anchors[i];
    if (q <= q1) {
      const double t = static_cast<double>(q - q0) / static_cast<double>(q1 - q0);
      return static_cast<int>(std::lround(qp0 + t * (qp1 - qp0)));
    }
  }
  return anchors.back().second;
}

struct H264Encoder::Impl {
  /// One input picture owned until libavc hands it back.
  struct InputPicture {
    std::vector<std::uint8_t> data;
    std::array<std::uint8_t*, 3> planes{};
  };
  struct InputTiming {
    std::int64_t pts = 0;
    std::int64_t duration = 0;
  };

  codec::VideoEncodeParams params;
  bool configured = false;
  bool failed = false;
  bool flushing = false;
  bool finished = false;

  std::vector<iv_mem_rec_t> records;
  iv_obj_t* handle = nullptr;
  std::uint32_t num_bframes = 0;

  Bytes avcc;
  std::vector<Bytes> header_sps;
  std::vector<Bytes> header_pps;

  std::list<InputPicture> inputs;
  std::vector<std::uint8_t> bitstream;
  std::map<std::uint32_t, InputTiming> timings;
  std::uint32_t next_index = 0;
  std::int64_t last_input_pts = std::numeric_limits<std::int64_t>::min();
  std::int64_t default_duration = 0;  ///< in timescale ticks

  /// Presentation times of all inputs so far, ascending (DTS generation).
  std::deque<std::int64_t> input_pts;
  std::uint64_t input_pts_base = 0;  ///< index of input_pts.front()
  std::uint64_t packets_out = 0;
  std::int64_t last_dts = std::numeric_limits<std::int64_t>::min();

  std::deque<codec::Packet> ready;

  ~Impl() { destroy(); }

  void destroy() {
    for (iv_mem_rec_t& record : records) {
      if (record.pv_base != nullptr) std::free(*(static_cast<void**>(record.pv_base) - 1));
      record.pv_base = nullptr;
    }
    records.clear();
    handle = nullptr;
  }

  [[nodiscard]] static void* allocate(std::uint32_t size, std::uint32_t alignment) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if ((alignment & (alignment - 1)) != 0) return nullptr;
    const std::size_t total = std::size_t{size} + alignment + sizeof(void*);
    void* raw = std::calloc(1, total);
    if (raw == nullptr) return nullptr;
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const std::uintptr_t aligned = (start + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1);
    *(reinterpret_cast<void**>(aligned) - 1) = raw;
    return reinterpret_cast<void*>(aligned);
  }

  template <class In, class Out>
  Result<void> control(In& in, Out& out, std::string_view what) {
    if (ih264e_api_function(handle, &in, &out) != IV_SUCCESS) {
      return fail(Status::InvalidArgument, "the H.264 encoder rejected the {} (error {:#x})", what, out.u4_error_code);
    }
    return {};
  }

  Result<void> create(std::uint32_t level, IVE_RC_MODE_T rc_mode) {
    iv_num_mem_rec_ip_t count_in{};
    iv_num_mem_rec_op_t count_out{};
    count_in.u4_size = sizeof(count_in);
    count_out.u4_size = sizeof(count_out);
    count_in.e_cmd = IV_CMD_GET_NUM_MEM_REC;
    if (ih264e_api_function(nullptr, &count_in, &count_out) != IV_SUCCESS || count_out.u4_num_mem_rec == 0 ||
        count_out.u4_num_mem_rec > 1024) {
      return fail(Status::Internal, "could not query the H.264 encoder");
    }
    records.assign(count_out.u4_num_mem_rec, iv_mem_rec_t{});
    for (iv_mem_rec_t& record : records) record.u4_size = sizeof(iv_mem_rec_t);

    const std::uint32_t max_ref = num_bframes > 0 ? 2 : 1;
    iv_fill_mem_rec_ip_t fill_in{};
    iv_fill_mem_rec_op_t fill_out{};
    fill_in.u4_size = sizeof(fill_in);
    fill_out.u4_size = sizeof(fill_out);
    fill_in.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
    fill_in.ps_mem_rec = records.data();
    fill_in.u4_num_mem_rec = static_cast<UWORD32>(records.size());
    fill_in.u4_max_wd = params.width;
    fill_in.u4_max_ht = params.height;
    fill_in.u4_max_level = level;
    fill_in.e_color_format = IV_YUV_420P;
    fill_in.u4_max_ref_cnt = max_ref;
    fill_in.u4_max_reorder_cnt = 0;  // libavc requires 0 even with B-frames
    fill_in.u4_max_srch_rng_x = 256;
    fill_in.u4_max_srch_rng_y = 256;
    fill_in.u4_keep_threads_active = 0;
    if (ih264e_api_function(nullptr, &fill_in, &fill_out) != IV_SUCCESS) {
      return fail(Status::InvalidArgument, "the H.264 encoder cannot handle {}x{} video (error {:#x})", params.width,
                  params.height, fill_out.u4_error_code);
    }
    for (iv_mem_rec_t& record : records) {
      record.pv_base = allocate(record.u4_mem_size, record.u4_mem_alignment);
      if (record.pv_base == nullptr) return fail(Status::Limit, "not enough memory for the H.264 encoder");
    }

    handle = static_cast<iv_obj_t*>(records[0].pv_base);
    handle->u4_size = sizeof(iv_obj_t);
    handle->pv_fxns = reinterpret_cast<void*>(&ih264e_api_function);
    ive_init_ip_t init_in{};
    ive_init_op_t init_out{};
    init_in.u4_size = sizeof(init_in);
    init_out.u4_size = sizeof(init_out);
    init_in.e_cmd = IV_CMD_INIT;
    init_in.u4_num_mem_rec = static_cast<UWORD32>(records.size());
    init_in.ps_mem_rec = records.data();
    init_in.u4_max_wd = params.width;
    init_in.u4_max_ht = params.height;
    init_in.u4_max_ref_cnt = max_ref;
    init_in.u4_max_reorder_cnt = 0;
    init_in.u4_max_level = level;
    init_in.e_inp_color_fmt = IV_YUV_420P;
    init_in.u4_enable_recon = 0;
    init_in.e_recon_color_fmt = IV_YUV_420P;
    init_in.e_rc_mode = rc_mode;
    init_in.u4_max_framerate = 120000;
    init_in.u4_max_bitrate = 240000000;
    init_in.u4_num_bframes = num_bframes;
    init_in.e_content_type = IV_PROGRESSIVE;
    init_in.u4_max_srch_rng_x = 256;
    init_in.u4_max_srch_rng_y = 256;
    init_in.e_slice_mode = IVE_SLICE_MODE_NONE;
    init_in.u4_slice_param = 0;
    init_in.e_arch = ARCH_NA;
    init_in.e_soc = SOC_GENERIC;
    init_in.u4_keep_threads_active = 0;
    if (ih264e_api_function(handle, &init_in, &init_out) != IV_SUCCESS) {
      return fail(Status::InvalidArgument, "could not start the H.264 encoder (error {:#x})", init_out.u4_error_code);
    }
    return {};
  }

  Result<void> apply_settings(IVE_RC_MODE_T rc_mode) {
    {
      ive_ctl_setdefault_ip_t in{};
      ive_ctl_setdefault_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SETDEFAULT);
      CVT_TRY(control(in, out, "defaults"));
    }
    {
      ive_ctl_set_num_cores_ip_t in{};
      ive_ctl_set_num_cores_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_NUM_CORES);
      in.u4_num_cores = 1;
      CVT_TRY(control(in, out, "core count"));
    }
    {
      ive_ctl_set_dimensions_ip_t in{};
      ive_ctl_set_dimensions_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_DIMENSIONS);
      in.u4_wd = params.width;
      in.u4_ht = params.height;
      CVT_TRY(control(in, out, "picture size"));
    }
    {
      // Used by rate control only; the exact rate goes into the VUI below.
      const double fps = params.fps.value();
      const auto whole = static_cast<UWORD32>(std::clamp(std::lround(fps > 0 ? fps : 30.0), 1L, 120L));
      ive_ctl_set_frame_rate_ip_t in{};
      ive_ctl_set_frame_rate_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_FRAMERATE);
      in.u4_src_frame_rate = whole;
      in.u4_tgt_frame_rate = whole;
      CVT_TRY(control(in, out, "frame rate"));
    }
    IVE_SPEED_CONFIG speed = IVE_NORMAL;
    UWORD32 search_x = 32;
    UWORD32 search_y = 24;
    switch (params.preset) {
      case codec::EncodePreset::Fast:
        speed = IVE_FAST;
        search_x = 16;
        search_y = 16;
        break;
      case codec::EncodePreset::Small:
        speed = IVE_SLOWEST;
        search_x = 64;
        search_y = 48;
        break;
      case codec::EncodePreset::Balanced:
        break;
    }
    {
      ive_ctl_set_ipe_params_ip_t in{};
      ive_ctl_set_ipe_params_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_IPE_PARAMS);
      in.u4_enable_intra_4x4 = 1;
      in.u4_enc_speed_preset = speed;
      in.u4_constrained_intra_pred = 0;
      CVT_TRY(control(in, out, "speed preset"));
    }
    {
      ive_ctl_set_me_params_ip_t in{};
      ive_ctl_set_me_params_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_ME_PARAMS);
      in.u4_enable_hpel = 1;
      in.u4_enable_qpel = 1;
      in.u4_enable_fast_sad = 0;
      in.u4_enable_alt_ref = 0;
      in.u4_me_speed_preset = 100;  // diamond search; the speed preset decides
      in.u4_srch_rng_x = search_x;
      in.u4_srch_rng_y = search_y;
      CVT_TRY(control(in, out, "motion search settings"));
    }
    const int qp = quality_to_qp(params.quality);
    {
      ive_ctl_set_qp_ip_t in{};
      ive_ctl_set_qp_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_QP);
      // libavc accepts quantisers 4..51; rate control may not go below 10.
      const UWORD32 floor_qp = rc_mode == IVE_RC_NONE ? 4 : 10;
      const auto clamp_qp = [floor_qp](int value) { return static_cast<UWORD32>(std::clamp(value, static_cast<int>(floor_qp), 51)); };
      in.u4_i_qp = clamp_qp(qp - 1);
      in.u4_p_qp = clamp_qp(qp);
      in.u4_b_qp = clamp_qp(qp + 1);
      in.u4_i_qp_min = floor_qp;
      in.u4_p_qp_min = floor_qp;
      in.u4_b_qp_min = floor_qp;
      in.u4_i_qp_max = 51;
      in.u4_p_qp_max = 51;
      in.u4_b_qp_max = 51;
      CVT_TRY(control(in, out, "quantiser settings"));
    }
    if (params.bitrate_kbps) {
      ive_ctl_set_bitrate_ip_t in{};
      ive_ctl_set_bitrate_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_BITRATE);
      in.u4_target_bitrate = static_cast<UWORD32>(std::min<std::uint64_t>(std::uint64_t{*params.bitrate_kbps} * 1000, 240000000));
      CVT_TRY(control(in, out, "bit rate"));
      ive_ctl_set_vbv_params_ip_t vbv_in{};
      ive_ctl_set_vbv_params_op_t vbv_out{};
      prepare(vbv_in, vbv_out, IVE_CMD_CTL_SET_VBV_PARAMS);
      vbv_in.u4_vbv_buffer_delay = 1000;
      vbv_in.u4_vbv_buf_size = 0;
      CVT_TRY(control(vbv_in, vbv_out, "buffer settings"));
    }
    {
      ive_ctl_set_air_params_ip_t in{};
      ive_ctl_set_air_params_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_AIR_PARAMS);
      in.e_air_mode = IVE_AIR_MODE_NONE;
      in.u4_air_refresh_period = 0;
      CVT_TRY(control(in, out, "intra refresh settings"));
    }
    {
      ive_ctl_set_gop_params_ip_t in{};
      ive_ctl_set_gop_params_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_GOP_PARAMS);
      in.u4_i_frm_interval = keyframe_interval();
      in.u4_idr_frm_interval = keyframe_interval();
      CVT_TRY(control(in, out, "keyframe interval"));
    }
    {
      ive_ctl_set_deblock_params_ip_t in{};
      ive_ctl_set_deblock_params_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_DEBLOCK_PARAMS);
      in.u4_disable_deblock_level = 0;
      CVT_TRY(control(in, out, "deblocking settings"));
    }
    {
      ih264e_vui_ip_t in{};
      ih264e_vui_op_t out{};
      in.u4_size = sizeof(in);
      out.u4_size = sizeof(out);
      in.e_cmd = IVE_CMD_VIDEO_CTL;
      in.e_sub_cmd = IVE_CMD_CTL_SET_VUI_PARAMS;
      const ColourCodes codes = colour_codes(params.color);
      in.u1_video_signal_type_present_flag = 1;
      in.u1_video_format = 5;  // unspecified
      in.u1_video_full_range_flag = params.color.range == ir::ColorRange::Full ? 1 : 0;
      in.u1_colour_description_present_flag = 1;
      in.u1_colour_primaries = codes.primaries;
      in.u1_transfer_characteristics = codes.transfer;
      in.u1_matrix_coefficients = codes.matrix;
      if (params.color.siting != ir::ChromaSiting::Left) {
        const UWORD8 type = params.color.siting == ir::ChromaSiting::Center ? 1 : 2;
        in.u1_chroma_loc_info_present_flag = 1;
        in.u1_chroma_sample_loc_type_top_field = type;
        in.u1_chroma_sample_loc_type_bottom_field = type;
      }
      // Timing: time_scale / (2 * num_units_in_tick) = fps.
      if (params.fps.num > 0 && params.fps.den > 0) {
        const std::int64_t divisor = std::gcd(params.fps.num, params.fps.den);
        const std::int64_t num = params.fps.num / divisor;
        const std::int64_t den = params.fps.den / divisor;
        if (num * 2 <= 0xFFFFFFFFll && den <= 0xFFFFFFFFll) {
          in.u1_vui_timing_info_present_flag = 1;
          in.u4_vui_num_units_in_tick = static_cast<UWORD32>(den);
          in.u4_vui_time_scale = static_cast<UWORD32>(num * 2);
          in.u1_fixed_frame_rate_flag = 0;
        }
      }
      in.u1_bitstream_restriction_flag = 1;
      in.u1_motion_vectors_over_pic_boundaries_flag = 1;
      in.u1_log2_max_mv_length_horizontal = 16;
      in.u1_log2_max_mv_length_vertical = 16;
      in.u1_num_reorder_frames = static_cast<UWORD8>(num_bframes > 0 ? 1 : 0);
      in.u1_max_dec_frame_buffering = static_cast<UWORD8>(num_bframes > 0 ? 2 : 1);
      CVT_TRY(control(in, out, "video usability information"));
    }
    {
      ive_ctl_set_profile_params_ip_t in{};
      ive_ctl_set_profile_params_op_t out{};
      prepare(in, out, IVE_CMD_CTL_SET_PROFILE_PARAMS);
      in.e_profile = IV_PROFILE_MAIN;
      in.u4_entropy_coding_mode = 1;  // CABAC
      CVT_TRY(control(in, out, "profile"));
    }
    return {};
  }

  [[nodiscard]] UWORD32 keyframe_interval() const {
    if (params.keyframe_interval > 0) return std::min<UWORD32>(params.keyframe_interval, 1000000);
    const double fps = params.fps.value();
    if (fps > 0) return static_cast<UWORD32>(std::clamp(std::lround(fps * 10.0), 1L, 300L));
    return 250;
  }

  Result<void> set_mode(IVE_ENC_MODE_T mode) {
    ive_ctl_set_enc_mode_ip_t in{};
    ive_ctl_set_enc_mode_op_t out{};
    prepare(in, out, IVE_CMD_CTL_SET_ENC_MODE);
    in.e_enc_mode = mode;
    return control(in, out, "mode");
  }

  /// One encode call. `picture` is null when flushing.
  Result<void> encode(InputPicture* picture, std::uint32_t index) {
    ive_video_encode_ip_t in{};
    ive_video_encode_op_t out{};
    in.u4_size = sizeof(in);
    out.u4_size = sizeof(out);
    in.e_cmd = IVE_CMD_VIDEO_ENCODE;
    in.s_inp_buf.u4_size = sizeof(iv_raw_buf_t);
    in.s_inp_buf.e_color_fmt = IV_YUV_420P;
    if (picture != nullptr) {
      const std::uint32_t cw = (params.width + 1) / 2;
      const std::uint32_t ch = (params.height + 1) / 2;
      for (std::size_t p = 0; p < 3; ++p) in.s_inp_buf.apv_bufs[p] = picture->planes[p];
      in.s_inp_buf.au4_wd[0] = params.width;
      in.s_inp_buf.au4_ht[0] = params.height;
      in.s_inp_buf.au4_strd[0] = params.width;
      for (std::size_t p = 1; p < 3; ++p) {
        in.s_inp_buf.au4_wd[p] = cw;
        in.s_inp_buf.au4_ht[p] = ch;
        in.s_inp_buf.au4_strd[p] = cw;
      }
      in.u4_is_last = 0;
    } else {
      in.u4_is_last = 1;
    }
    in.u4_timestamp_low = index;
    in.u4_timestamp_high = 0;
    in.s_out_buf.u4_size = sizeof(iv_bits_buf_t);
    in.s_out_buf.pv_buf = bitstream.data();
    in.s_out_buf.u4_bufsize = static_cast<UWORD32>(bitstream.size());
    in.s_out_buf.u4_bytes = 0;
    if (ih264e_api_function(handle, &in, &out) != IV_SUCCESS) {
      return fail(Status::Encode, "the H.264 encoder failed (error {:#x})", out.u4_error_code);
    }
    // Input pictures libavc no longer needs.
    if (out.s_inp_buf.apv_bufs[0] != nullptr) {
      inputs.remove_if([&](const InputPicture& held) { return held.planes[0] == out.s_inp_buf.apv_bufs[0]; });
    }
    if (out.u4_is_last != 0) finished = true;
    if (out.s_out_buf.u4_bytes > 0 && out.s_out_buf.u4_bytes <= bitstream.size()) {
      CVT_TRY(emit(ByteView(reinterpret_cast<const std::byte*>(bitstream.data()), out.s_out_buf.u4_bytes),
                   out.u4_timestamp_low, out.u4_encoded_frame_type == IV_IDR_FRAME));
    }
    return {};
  }

  Result<void> emit(ByteView annexb, std::uint32_t index, bool idr) {
    std::vector<ByteView> nals = split_annexb(annexb);
    // Parameter sets identical to the avcC ones are dropped from the samples.
    std::erase_if(nals, [&](ByteView nal) {
      const std::uint8_t type = nal_type(nal);
      const auto same = [&](const std::vector<Bytes>& sets) {
        return std::any_of(sets.begin(), sets.end(),
                           [&](const Bytes& set) { return std::equal(set.begin(), set.end(), nal.begin(), nal.end()); });
      };
      return (type == nal_sps && same(header_sps)) || (type == nal_pps && same(header_pps)) || type == nal_aud;
    });
    bool has_picture = false;
    for (const ByteView nal : nals) has_picture = has_picture || is_vcl(nal_type(nal));
    if (!has_picture) return {};
    CVT_TRY_ASSIGN(Bytes sample, to_length_prefixed(nals, 4));

    codec::Packet packet;
    packet.bytes = std::move(sample);
    packet.keyframe = idr;
    InputTiming timing;
    if (const auto found = timings.find(index); found != timings.end()) {
      timing = found->second;
      timings.erase(found);
    } else {
      return fail(Status::Internal, "the H.264 encoder returned an unknown picture");
    }
    packet.pts = timing.pts;
    packet.duration = timing.duration;
    // DTS: with B-frames (one picture of reordering), packet k decodes at the
    // presentation time of input k-1; the first one a frame early.
    std::int64_t dts = timing.pts;
    if (num_bframes > 0) {
      const std::uint64_t k = packets_out;
      if (k == 0) {
        const std::int64_t step = input_pts.size() >= 2 ? input_pts[1] - input_pts[0] : default_duration;
        dts = input_pts.empty() ? timing.pts - step : input_pts.front() - std::max<std::int64_t>(step, 1);
      } else if (k - 1 >= input_pts_base && k - 1 - input_pts_base < input_pts.size()) {
        dts = input_pts[static_cast<std::size_t>(k - 1 - input_pts_base)];
      }
      // Drop presentation times that no later packet will need.
      while (!input_pts.empty() && input_pts_base + 1 < k) {
        input_pts.pop_front();
        ++input_pts_base;
      }
    }
    if (last_dts != std::numeric_limits<std::int64_t>::min() && dts <= last_dts) dts = last_dts + 1;
    dts = std::min(dts, packet.pts);
    packet.dts = dts;
    last_dts = dts;
    ++packets_out;
    ready.push_back(std::move(packet));
    return {};
  }

  Result<void> header() {
    CVT_TRY(set_mode(IVE_ENC_MODE_HEADER));
    ive_video_encode_ip_t in{};
    ive_video_encode_op_t out{};
    in.u4_size = sizeof(in);
    out.u4_size = sizeof(out);
    in.e_cmd = IVE_CMD_VIDEO_ENCODE;
    in.s_inp_buf.u4_size = sizeof(iv_raw_buf_t);
    in.s_inp_buf.e_color_fmt = IV_YUV_420P;
    in.s_out_buf.u4_size = sizeof(iv_bits_buf_t);
    in.s_out_buf.pv_buf = bitstream.data();
    in.s_out_buf.u4_bufsize = static_cast<UWORD32>(bitstream.size());
    if (ih264e_api_function(handle, &in, &out) != IV_SUCCESS || out.s_out_buf.u4_bytes == 0 ||
        out.s_out_buf.u4_bytes > bitstream.size()) {
      return fail(Status::Encode, "the H.264 encoder could not write its parameter sets (error {:#x})",
                  out.u4_error_code);
    }
    const ByteView headers(reinterpret_cast<const std::byte*>(bitstream.data()), out.s_out_buf.u4_bytes);
    std::vector<ByteView> sps;
    std::vector<ByteView> pps;
    for (const ByteView nal : split_annexb(headers)) {
      if (nal_type(nal) == nal_sps) {
        sps.push_back(nal);
        header_sps.emplace_back(nal.begin(), nal.end());
      } else if (nal_type(nal) == nal_pps) {
        pps.push_back(nal);
        header_pps.emplace_back(nal.begin(), nal.end());
      }
    }
    CVT_TRY_ASSIGN(avcc, build_avcc(sps, pps, 4));
    return set_mode(IVE_ENC_MODE_PICTURE);
  }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

Result<void> H264Encoder::configure(const codec::VideoEncodeParams& params) {
  Impl& e = *impl_;
  if (e.configured) return fail(Status::InvalidArgument, "the H.264 encoder is already configured");
  e.configured = true;
  e.failed = true;  // until configuration succeeds
  if (params.width == 0 || params.height == 0) return fail(Status::InvalidArgument, "H.264 encoding needs a picture size");
  if (params.width < 16 || params.height < 16) {
    return fail(Status::InvalidArgument, "the H.264 encoder needs pictures of at least 16x16 pixels (got {}x{})",
                params.width, params.height);
  }
  if (params.width > max_encode_dimension || params.height > max_encode_dimension) {
    return fail(Status::InvalidArgument, "the H.264 encoder supports pictures up to {}x{} pixels (got {}x{})",
                max_encode_dimension, max_encode_dimension, params.width, params.height);
  }
  if ((params.width & 1) != 0 || (params.height & 1) != 0) {
    return fail(Status::InvalidArgument, "H.264 4:2:0 video needs an even width and height (got {}x{})", params.width,
                params.height);
  }
  e.params = params;
  if (e.params.timescale == 0) e.params.timescale = 90000;
  e.params.quality = std::clamp(e.params.quality, 1, 100);
  if (e.params.bitrate_kbps && *e.params.bitrate_kbps == 0) e.params.bitrate_kbps.reset();
  e.num_bframes = params.preset == codec::EncodePreset::Fast ? 0 : 2;
  if (e.params.keyframe_interval == 1) e.num_bframes = 0;  // intra only

  const double fps = e.params.fps.value();
  const std::int64_t duration_ms = fps > 0 ? std::max<std::int64_t>(1, std::llround(1000.0 / fps)) : 40;
  e.default_duration = fps > 0 ? std::max<std::int64_t>(1, std::llround(e.params.timescale / fps))
                               : ms_to_ticks(duration_ms, e.params.timescale);
  const std::uint32_t level = pick_level(params.width, params.height, fps);
  const IVE_RC_MODE_T rc_mode = e.params.bitrate_kbps ? IVE_RC_STORAGE : IVE_RC_NONE;

  // Worst case for one coded picture, plus headers.
  const std::uint64_t pixels = std::uint64_t{params.width} * params.height;
  e.bitstream.assign(static_cast<std::size_t>(pixels * 3 + 65536), 0);

  CVT_TRY(e.create(level, rc_mode));
  CVT_TRY(e.apply_settings(rc_mode));
  CVT_TRY(e.header());
  e.failed = false;
  return {};
}

Result<void> H264Encoder::send_frame(const codec::VideoFrame& frame) {
  Impl& e = *impl_;
  if (!e.configured || e.failed) return fail(Status::InvalidArgument, "the H.264 encoder is not ready");
  if (e.flushing) return fail(Status::InvalidArgument, "the H.264 encoder was already flushed");
  if (frame.width != e.params.width || frame.height != e.params.height) {
    return fail(Status::InvalidArgument, "frame size {}x{} differs from the configured {}x{}", frame.width,
                frame.height, e.params.width, e.params.height);
  }
  const codec::VideoFrame* source = &frame;
  codec::VideoFrame converted;
  if (frame.format != ir::PixelFormat::I420) {
    Result<codec::VideoFrame> result = codec::convert_frame(frame, ir::PixelFormat::I420, e.params.color);
    if (!result) return fail(result.error());
    converted = std::move(*result);
    source = &converted;
  } else if (!frame.valid()) {
    return fail(Status::InvalidArgument, "invalid video frame");
  }

  // Own a tightly packed copy until libavc releases it.
  Impl::InputPicture picture;
  const std::size_t sizes[3] = {std::size_t{e.params.width} * e.params.height,
                                std::size_t{(e.params.width + 1) / 2} * ((e.params.height + 1) / 2),
                                std::size_t{(e.params.width + 1) / 2} * ((e.params.height + 1) / 2)};
  picture.data.resize(sizes[0] + sizes[1] + sizes[2]);
  std::size_t offset = 0;
  for (std::size_t p = 0; p < 3; ++p) {
    picture.planes[p] = picture.data.data() + offset;
    const std::size_t rows = ir::plane_height(ir::PixelFormat::I420, p, e.params.height);
    const std::size_t row_bytes = ir::plane_row_bytes(ir::PixelFormat::I420, p, e.params.width);
    for (std::size_t r = 0; r < rows; ++r) {
      ::cvt::util::copy_bytes(picture.planes[p] + r * row_bytes, source->planes[p].data.data() + r * source->planes[p].stride,
                  row_bytes);
    }
    offset += sizes[p];
  }

  std::int64_t pts = ms_to_ticks(frame.pts_ms, e.params.timescale);
  std::int64_t duration = frame.duration_ms > 0 ? ms_to_ticks(frame.duration_ms, e.params.timescale) : e.default_duration;
  if (e.last_input_pts != std::numeric_limits<std::int64_t>::min() && pts <= e.last_input_pts) {
    pts = e.last_input_pts + (duration > 0 ? duration : e.default_duration);
  }
  if (duration <= 0) duration = e.default_duration;
  e.last_input_pts = pts;
  e.input_pts.push_back(pts);

  const std::uint32_t index = e.next_index++;
  e.timings[index] = {pts, duration};
  e.inputs.push_back(std::move(picture));
  Impl::InputPicture* held = &e.inputs.back();
  if (Result<void> encoded = e.encode(held, index); !encoded) {
    e.failed = true;
    return encoded;
  }
  return {};
}

Result<std::optional<codec::Packet>> H264Encoder::receive_packet() {
  Impl& e = *impl_;
  if (!e.configured || e.failed) return fail(Status::InvalidArgument, "the H.264 encoder is not ready");
  if (e.ready.empty()) return std::optional<codec::Packet>{};
  codec::Packet packet = std::move(e.ready.front());
  e.ready.pop_front();
  return std::optional<codec::Packet>{std::move(packet)};
}

Bytes H264Encoder::extradata() const { return impl_->avcc; }

Result<void> H264Encoder::flush() {
  Impl& e = *impl_;
  if (!e.configured || e.failed) return fail(Status::InvalidArgument, "the H.264 encoder is not ready");
  if (e.flushing) return {};
  e.flushing = true;
  // Each call releases at most one delayed picture.
  for (std::uint32_t i = 0; i < 64 && !e.finished && !e.timings.empty(); ++i) {
    if (Result<void> encoded = e.encode(nullptr, 0); !encoded) {
      e.failed = true;
      return encoded;
    }
  }
  return {};
}

}  // namespace cvt::h264
