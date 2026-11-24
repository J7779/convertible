// Convertible core - codec_h264 family: H.264 decoder (libavc wrapper).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <utility>

#include "util/bytes.hpp"
#include "codec/ids.hpp"
#include "families/codec_h264/h264_bitstream.hpp"
#include "families/codec_h264/h264_codec.hpp"

// clang-format off
#include "ih264_typedefs.h"
#include "ih264d.h"
// clang-format on

namespace cvt::h264 {

namespace {

// libavc allocates through these callbacks; the original pointer is kept
// just before the aligned block.
void* aligned_alloc_cb(void* /*context*/, WORD32 alignment, WORD32 size) {
  if (size <= 0) size = 1;
  if (alignment < static_cast<WORD32>(sizeof(void*))) alignment = static_cast<WORD32>(sizeof(void*));
  const std::size_t total = static_cast<std::size_t>(size) + static_cast<std::size_t>(alignment) + sizeof(void*);
  void* raw = std::malloc(total);
  if (raw == nullptr) return nullptr;
  const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
  const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment) - 1;
  const std::uintptr_t aligned = (start + mask) & ~mask;
  void** slot = reinterpret_cast<void**>(aligned) - 1;
  *slot = raw;
  // libavc reads some buffers before writing them in error paths; start clean.
  ::cvt::util::fill_bytes(reinterpret_cast<void*>(aligned), 0, static_cast<std::size_t>(size));
  return reinterpret_cast<void*>(aligned);
}

void aligned_free_cb(void* /*context*/, void* block) {
  if (block == nullptr) return;
  std::free(*(reinterpret_cast<void**>(block) - 1));
}

/// ticks -> milliseconds, rounded to nearest, without overflow.
[[nodiscard]] std::int64_t ticks_to_ms(std::int64_t ticks, std::uint32_t timescale) noexcept {
  if (timescale == 0) return ticks;
  const std::int64_t scale = timescale;
  std::int64_t whole = ticks / scale;
  std::int64_t rest = ticks % scale;
  if (rest < 0) {
    rest += scale;
    --whole;
  }
  return whole * 1000 + (rest * 1000 + scale / 2) / scale;
}

constexpr std::size_t max_pending_timestamps = 512;
/// ERROR_FEATURE_UNAVAIL of decoder/ih264d_error_handler.h (not a public header).
constexpr UWORD32 feature_unavailable = 0x59;
/// Padding after the bitstream handed to libavc (it reads whole words).
constexpr std::size_t input_padding = 16;

}  // namespace

struct H264Decoder::Impl {
  struct Timing {
    std::int64_t pts_ms = 0;
    std::int64_t duration_ms = 0;
  };

  codec::CodecParams params;
  bool configured = false;
  bool failed = false;
  bool flushed = false;
  /// 0: Annex B input; 1, 2 or 4: length-prefixed with that size.
  int length_size = 0;
  bool length_size_known = false;

  iv_obj_t* handle = nullptr;
  bool headers_decoded = false;
  /// Parameter sets (raw NAL units) seen so far, by type and id, replayed
  /// after a reset.
  std::map<std::uint32_t, Bytes> sps_sets;
  std::map<std::uint32_t, Bytes> pps_sets;
  std::optional<Sps> sps;  ///< the most recent valid SPS

  Bytes stream;  ///< Annex B buffer for one decode run (+ padding)
  Bytes header_stream;  ///< parameter sets replayed by decode_headers()
  std::vector<std::uint8_t> picture;  ///< output buffer, I420
  std::array<std::uint32_t, 3> plane_sizes{};

  std::map<std::uint32_t, Timing> timings;
  std::uint32_t next_ts = 1;

  std::deque<codec::VideoFrame> pending;
  std::int64_t last_pts_ms = std::numeric_limits<std::int64_t>::min();
  std::int64_t last_duration_ms = 0;

  std::vector<std::string> warnings;
  std::set<std::string> warned;

  ~Impl() { destroy(); }

  void warn(std::string message) {
    if (warned.insert(message).second) warnings.push_back(std::move(message));
  }

  void destroy() {
    if (handle == nullptr) return;
    ivd_delete_ip_t in{};
    ivd_delete_op_t out{};
    in.e_cmd = IVD_CMD_DELETE;
    in.u4_size = sizeof(in);
    out.u4_size = sizeof(out);
    ih264d_api_function(handle, &in, &out);
    handle = nullptr;
  }

  Result<void> create() {
    ih264d_create_ip_t in{};
    ih264d_create_op_t out{};
    in.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    in.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    in.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
    in.s_ivd_create_ip_t.pf_aligned_alloc = aligned_alloc_cb;
    in.s_ivd_create_ip_t.pf_aligned_free = aligned_free_cb;
    in.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
    in.s_ivd_create_ip_t.u4_size = sizeof(in);
    in.u4_enable_frame_info = 0;
    in.u4_keep_threads_active = 0;
    out.s_ivd_create_op_t.u4_size = sizeof(out);
    if (ih264d_api_function(nullptr, &in, &out) != IV_SUCCESS || out.s_ivd_create_op_t.pv_handle == nullptr) {
      return fail(Status::Internal, "could not create the H.264 decoder (error {:#x})",
                  out.s_ivd_create_op_t.u4_error_code);
    }
    handle = static_cast<iv_obj_t*>(out.s_ivd_create_op_t.pv_handle);
    handle->pv_fxns = reinterpret_cast<void*>(&ih264d_api_function);
    handle->u4_size = sizeof(iv_obj_t);

    ih264d_ctl_set_num_cores_ip_t cores_in{};
    ih264d_ctl_set_num_cores_op_t cores_out{};
    cores_in.e_cmd = IVD_CMD_VIDEO_CTL;
    cores_in.e_sub_cmd = static_cast<IVD_CONTROL_API_COMMAND_TYPE_T>(IH264D_CMD_CTL_SET_NUM_CORES);
    cores_in.u4_num_cores = 1;
    cores_in.u4_size = sizeof(cores_in);
    cores_out.u4_size = sizeof(cores_out);
    if (ih264d_api_function(handle, &cores_in, &cores_out) != IV_SUCCESS) {
      return fail(Status::Internal, "could not configure the H.264 decoder");
    }
    headers_decoded = false;
    return set_mode(IVD_DECODE_HEADER);
  }

  Result<void> set_mode(IVD_VIDEO_DECODE_MODE_T mode) {
    ivd_ctl_set_config_ip_t in{};
    ivd_ctl_set_config_op_t out{};
    in.u4_disp_wd = 0;
    in.e_frm_skip_mode = IVD_SKIP_NONE;
    in.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    in.e_vid_dec_mode = mode;
    in.e_cmd = IVD_CMD_VIDEO_CTL;
    in.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    in.u4_size = sizeof(in);
    out.u4_size = sizeof(out);
    if (ih264d_api_function(handle, &in, &out) != IV_SUCCESS) {
      return fail(Status::Internal, "could not configure the H.264 decoder");
    }
    return {};
  }

  /// Ask libavc for the output buffer sizes once the headers are known.
  Result<void> allocate_picture() {
    ivd_ctl_getbufinfo_ip_t in{};
    ivd_ctl_getbufinfo_op_t out{};
    in.e_cmd = IVD_CMD_VIDEO_CTL;
    in.e_sub_cmd = IVD_CMD_CTL_GETBUFINFO;
    in.u4_size = sizeof(in);
    out.u4_size = sizeof(out);
    if (ih264d_api_function(handle, &in, &out) != IV_SUCCESS || out.u4_min_num_out_bufs < 3) {
      return fail(Status::Decode, "the H.264 decoder could not size its output");
    }
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < 3; ++i) {
      plane_sizes[i] = out.u4_min_out_buf_size[i];
      total += plane_sizes[i];
    }
    if (total == 0 || total > (std::uint64_t{max_decode_dimension} * max_decode_dimension * 3) / 2 + 4096) {
      return fail(Status::Decode, "the H.264 decoder reported an invalid picture size");
    }
    picture.assign(static_cast<std::size_t>(total), 0);
    return {};
  }

  /// Record parameter sets and refuse streams libavc cannot decode, with a
  /// message a user can act on.
  Result<void> inspect(ByteView nal) {
    const std::uint8_t type = nal_type(nal);
    if (type == nal_sps) {
      const Result<Sps> parsed = parse_sps(nal);
      if (!parsed) {
        warn("a damaged H.264 sequence parameter set was ignored");
        return {};
      }
      if (const auto reason = unsupported_reason(*parsed)) return fail(Status::Decode, "{}", *reason);
      if (parsed->coded_width > max_decode_dimension || parsed->coded_height > max_decode_dimension) {
        return fail(Status::Decode, "H.264 video larger than {}x{} pixels ({}x{}) is not supported", max_decode_dimension,
                    max_decode_dimension, parsed->coded_width, parsed->coded_height);
      }
      if (params.max_pixels > 0 && std::uint64_t{parsed->width} * parsed->height > params.max_pixels) {
        return fail(Status::Limit, "H.264 picture of {}x{} exceeds the limit of {} pixels", parsed->width, parsed->height,
                    params.max_pixels);
      }
      sps = *parsed;
      sps_sets[parsed->id] = Bytes(nal.begin(), nal.end());
    } else if (type == nal_pps) {
      const Bytes rbsp = nal_to_rbsp(nal);
      BitReader reader(rbsp);
      const std::uint32_t id = reader.ue();
      if (!reader.overrun() && id < 256) pps_sets[id] = Bytes(nal.begin(), nal.end());
    } else if (type == nal_subset_sps || type == nal_slice_extension) {
      warn("H.264 scalable/multiview layers were ignored; only the base layer is decoded");
    }
    return {};
  }

  /// The NAL units of a packet, in the framing detected for this stream.
  Result<std::vector<ByteView>> packet_nals(ByteView data) {
    if (!length_size_known) {
      // No avcC: decide from the first packet, then stick with it.
      length_size = starts_with_start_code(data) ? 0 : 4;
      if (length_size == 4 && !split_length_prefixed(data, 4)) length_size = 0;
      length_size_known = true;
    }
    if (length_size == 0) return split_annexb(data);
    Result<std::vector<ByteView>> nals = split_length_prefixed(data, length_size);
    if (!nals && starts_with_start_code(data)) return split_annexb(data);  // mixed framing in the wild
    return nals;
  }

  void build_stream(Bytes& into, std::span<const ByteView> nals, bool with_parameter_sets) {
    into.clear();
    const auto add = [&into](ByteView nal) {
      into.insert(into.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}});
      into.insert(into.end(), nal.begin(), nal.end());
    };
    if (with_parameter_sets) {
      for (const auto& [id, set] : sps_sets) add(set);
      for (const auto& [id, set] : pps_sets) add(set);
    }
    for (const ByteView nal : nals) add(nal);
  }

  /// Decode the headers held in sps_sets/pps_sets.
  Result<void> decode_headers() {
    if (sps_sets.empty() || pps_sets.empty()) return {};
    build_stream(header_stream, {}, true);
    const std::size_t size = header_stream.size();
    header_stream.resize(size + input_padding);
    std::size_t pos = 0;
    for (int calls = 0; pos < size && calls < 64; ++calls) {
      ivd_video_decode_ip_t in{};
      ivd_video_decode_op_t out{};
      in.e_cmd = IVD_CMD_VIDEO_DECODE;
      in.u4_ts = 0;
      in.pv_stream_buffer = header_stream.data() + pos;
      in.u4_num_Bytes = static_cast<UWORD32>(size - pos);
      in.u4_size = sizeof(in);
      out.u4_size = sizeof(out);
      ih264d_api_function(handle, &in, &out);
      if (out.u4_pic_wd != 0 && out.u4_pic_ht != 0) {
        headers_decoded = true;
        break;
      }
      if (out.u4_num_bytes_consumed == 0) break;
      pos += out.u4_num_bytes_consumed;
    }
    if (!headers_decoded) return {};
    CVT_TRY(allocate_picture());
    return set_mode(IVD_DECODE_FRAME);
  }

  /// Drop everything and start again (resolution change, fatal error).
  Result<void> restart() {
    CVT_TRY(drain());
    destroy();
    return create();
  }

  void take_output(const ivd_video_decode_op_t& out) {
    if (out.u4_output_present == 0) return;
    const iv_yuv_buf_t& buf = out.s_disp_frm_buf;
    const std::uint32_t width = buf.u4_y_wd;
    const std::uint32_t height = buf.u4_y_ht;
    if (width == 0 || height == 0 || width > max_decode_dimension || height > max_decode_dimension) return;
    const auto* base = picture.data();
    const auto* end = base + picture.size();
    const auto* y = static_cast<const std::uint8_t*>(buf.pv_y_buf);
    const auto* u = static_cast<const std::uint8_t*>(buf.pv_u_buf);
    const auto* v = static_cast<const std::uint8_t*>(buf.pv_v_buf);
    codec::VideoFrame frame = codec::VideoFrame::allocate(ir::PixelFormat::I420, width, height);
    const std::uint8_t* sources[3] = {y, u, v};
    const std::size_t strides[3] = {buf.u4_y_strd, buf.u4_u_strd, buf.u4_v_strd};
    for (std::size_t p = 0; p < 3; ++p) {
      const std::size_t rows = ir::plane_height(frame.format, p, height);
      const std::size_t row_bytes = ir::plane_row_bytes(frame.format, p, width);
      const std::uint8_t* src = sources[p];
      // The planes must lie inside our buffer; anything else is a decoder bug.
      if (src == nullptr || src < base || strides[p] < row_bytes ||
          static_cast<std::size_t>(end - src) < strides[p] * (rows - 1) + row_bytes) {
        warn("an H.264 picture with an invalid layout was dropped");
        return;
      }
      for (std::size_t r = 0; r < rows; ++r) {
        ::cvt::util::copy_bytes(frame.planes[p].data.data() + r * frame.planes[p].stride, src + r * strides[p], row_bytes);
      }
    }
    if (sps) frame.color = sps->color_info();
    Timing timing;
    bool known = false;
    if (const auto found = timings.find(out.u4_ts); found != timings.end()) {
      timing = found->second;
      known = true;
      timings.erase(found);
    }
    frame.pts_ms = known ? timing.pts_ms : last_pts_ms;
    frame.duration_ms = known ? timing.duration_ms : 0;
    pending.push_back(std::move(frame));
  }

  void note_errors(const ivd_video_decode_op_t& out) {
    const UWORD32 code = out.u4_error_code;
    if (IS_IVD_CONCEALMENT_APPLIED(code) || IS_IVD_CORRUPTEDDATA_ERROR(code) || IS_IVD_CORRUPTEDHEADER_ERROR(code)) {
      warn("the H.264 stream is damaged; affected pictures were concealed");
    } else if (IS_IVD_INSUFFICIENTDATA_ERROR(code) || IS_IVD_INCOMPLETE_BITSTREAM_ERROR(code)) {
      warn("the H.264 stream is incomplete; some pictures may be missing");
    } else if ((code & IVD_ERROR_MASK) == IVD_DEC_REF_BUF_NULL || (code & IVD_ERROR_MASK) == IVD_DEC_FRM_SKIPPED) {
      warn("H.264 pictures without their reference pictures were skipped");
    }
  }

  /// Feed one Annex B buffer (already in `stream`) in frame mode.
  Result<void> decode_stream(std::uint32_t ts, std::size_t nal_count) {
    const std::size_t size = stream.size();
    stream.resize(size + input_padding);
    std::size_t pos = 0;
    const std::size_t max_calls = nal_count + 4;
    for (std::size_t calls = 0; pos < size && calls < max_calls; ++calls) {
      ivd_video_decode_ip_t in{};
      ivd_video_decode_op_t out{};
      in.e_cmd = IVD_CMD_VIDEO_DECODE;
      in.u4_ts = ts;
      in.pv_stream_buffer = stream.data() + pos;
      in.u4_num_Bytes = static_cast<UWORD32>(size - pos);
      in.u4_size = sizeof(in);
      in.s_out_buffer.u4_num_bufs = 3;
      in.s_out_buffer.pu1_bufs[0] = picture.data();
      in.s_out_buffer.pu1_bufs[1] = picture.data() + plane_sizes[0];
      in.s_out_buffer.pu1_bufs[2] = picture.data() + plane_sizes[0] + plane_sizes[1];
      for (std::size_t i = 0; i < 3; ++i) in.s_out_buffer.u4_min_out_buf_size[i] = plane_sizes[i];
      out.u4_size = sizeof(out);
      const IV_API_CALL_STATUS_T status = ih264d_api_function(handle, &in, &out);
      if (status != IV_SUCCESS && (out.u4_error_code & IVD_ERROR_MASK) == IVD_RES_CHANGED) {
        take_output(out);
        // New stream geometry: release what is buffered, then start over with
        // the new parameter sets, which are already recorded.
        CVT_TRY(restart());
        CVT_TRY(decode_headers());
        if (!headers_decoded) return {};
        continue;
      }
      take_output(out);
      if (status != IV_SUCCESS) {
        if (IS_IVD_FATAL_ERROR(out.u4_error_code) && (out.u4_error_code & IVD_ERROR_MASK) == feature_unavailable) {
          return fail(Status::Decode, "the H.264 stream uses a feature that is not supported yet");
        }
        note_errors(out);
        if (IS_IVD_FATAL_ERROR(out.u4_error_code)) {
          warn("the H.264 decoder lost synchronisation and restarted at the next decodable picture");
          CVT_TRY(restart());
          CVT_TRY(decode_headers());
          return {};
        }
      } else {
        note_errors(out);
      }
      const std::size_t consumed = out.u4_num_bytes_consumed;
      if (consumed == 0 || consumed > size - pos) break;
      pos += consumed;
    }
    return {};
  }

  /// Release every picture the decoder still holds.
  Result<void> drain() {
    if (handle == nullptr || !headers_decoded) return {};
    ivd_ctl_flush_ip_t in{};
    ivd_ctl_flush_op_t out{};
    in.e_cmd = IVD_CMD_VIDEO_CTL;
    in.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    in.u4_size = sizeof(in);
    out.u4_size = sizeof(out);
    if (ih264d_api_function(handle, &in, &out) != IV_SUCCESS) return {};
    std::array<std::byte, input_padding> empty{};
    for (int i = 0; i < 64; ++i) {
      ivd_video_decode_ip_t dec_in{};
      ivd_video_decode_op_t dec_out{};
      dec_in.e_cmd = IVD_CMD_VIDEO_DECODE;
      dec_in.u4_ts = 0;
      dec_in.pv_stream_buffer = empty.data();
      dec_in.u4_num_Bytes = 0;
      dec_in.u4_size = sizeof(dec_in);
      dec_in.s_out_buffer.u4_num_bufs = 3;
      dec_in.s_out_buffer.pu1_bufs[0] = picture.data();
      dec_in.s_out_buffer.pu1_bufs[1] = picture.data() + plane_sizes[0];
      dec_in.s_out_buffer.pu1_bufs[2] = picture.data() + plane_sizes[0] + plane_sizes[1];
      for (std::size_t p = 0; p < 3; ++p) dec_in.s_out_buffer.u4_min_out_buf_size[p] = plane_sizes[p];
      dec_out.u4_size = sizeof(dec_out);
      ih264d_api_function(handle, &dec_in, &dec_out);
      if (dec_out.u4_output_present == 0) break;
      take_output(dec_out);
    }
    return {};
  }

  /// The fallback frame duration: VUI frame rate, else the last one seen, else 25 fps.
  [[nodiscard]] std::int64_t default_duration() const {
    if (sps) {
      const ir::Rational rate = sps->frame_rate();
      if (rate.num > 0 && rate.den > 0) {
        const std::int64_t ms = (1000 * rate.den + rate.num / 2) / rate.num;
        if (ms > 0) return ms;
      }
    }
    if (last_duration_ms > 0) return last_duration_ms;
    return 40;
  }

  /// The next frame to hand out, with timestamps made strictly increasing
  /// and durations filled in; held back by one frame so that a missing
  /// duration can be derived from the next timestamp.
  std::optional<codec::VideoFrame> pop_ready() {
    if (pending.empty() || (pending.size() < 2 && !flushed)) return std::nullopt;
    codec::VideoFrame frame = std::move(pending.front());
    pending.pop_front();
    if (last_pts_ms != std::numeric_limits<std::int64_t>::min() && frame.pts_ms <= last_pts_ms) {
      frame.pts_ms = last_pts_ms + (last_duration_ms > 0 ? last_duration_ms : default_duration());
    }
    if (frame.duration_ms <= 0) {
      if (!pending.empty() && pending.front().pts_ms > frame.pts_ms) {
        frame.duration_ms = pending.front().pts_ms - frame.pts_ms;
      } else {
        frame.duration_ms = default_duration();
      }
    }
    last_pts_ms = frame.pts_ms;
    last_duration_ms = frame.duration_ms;
    return frame;
  }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

Result<void> H264Decoder::configure(const codec::CodecParams& params) {
  Impl& d = *impl_;
  if (d.configured) return fail(Status::InvalidArgument, "the H.264 decoder is already configured");
  d.configured = true;
  d.params = params;
  if (d.params.timescale == 0) d.params.timescale = 1000;
  const ByteView extradata = params.extradata;
  if (is_avcc(extradata)) {
    const Result<AvcConfig> config = parse_avcc(extradata);
    if (!config) {
      d.failed = true;
      return fail(config.error());
    }
    d.length_size = config->length_size;
    d.length_size_known = true;
    for (const Bytes& set : config->sps) {
      if (Result<void> inspected = d.inspect(set); !inspected) {
        d.failed = true;
        return inspected;
      }
    }
    for (const Bytes& set : config->pps) (void)d.inspect(set);
  } else if (!extradata.empty()) {
    for (const ByteView nal : split_annexb(extradata)) {
      if (Result<void> inspected = d.inspect(nal); !inspected) {
        d.failed = true;
        return inspected;
      }
    }
  }
  if (Result<void> created = d.create(); !created) {
    d.failed = true;
    return created;
  }
  return {};
}

Result<void> H264Decoder::send_packet(const codec::Packet& packet) {
  Impl& d = *impl_;
  if (!d.configured || d.failed) return fail(Status::InvalidArgument, "the H.264 decoder is not ready");
  if (d.flushed) return fail(Status::InvalidArgument, "the H.264 decoder was already flushed");
  if (packet.bytes.empty()) return {};
  Result<std::vector<ByteView>> nals = d.packet_nals(packet.bytes);
  if (!nals) {
    d.warn("a damaged H.264 packet was skipped");
    return {};
  }
  for (const ByteView nal : *nals) {
    if (Result<void> inspected = d.inspect(nal); !inspected) {
      d.failed = true;
      return inspected;
    }
  }
  if (!d.headers_decoded) {
    if (Result<void> headers = d.decode_headers(); !headers) {
      d.failed = true;
      return headers;
    }
    if (!d.headers_decoded) {
      d.warn("H.264 data before the first parameter sets was skipped");
      return {};
    }
  }

  const std::uint32_t ts = d.next_ts++;
  if (d.next_ts == 0) d.next_ts = 1;
  Impl::Timing timing;
  timing.pts_ms = ticks_to_ms(packet.pts, d.params.timescale);
  timing.duration_ms = packet.duration > 0 ? ticks_to_ms(packet.duration, d.params.timescale) : 0;
  d.timings[ts] = timing;
  while (d.timings.size() > max_pending_timestamps) d.timings.erase(d.timings.begin());

  d.build_stream(d.stream, *nals, false);
  if (Result<void> decoded = d.decode_stream(ts, nals->size()); !decoded) {
    d.failed = true;
    return decoded;
  }
  return {};
}

Result<std::optional<codec::VideoFrame>> H264Decoder::receive_frame() {
  Impl& d = *impl_;
  if (!d.configured || d.failed) return fail(Status::InvalidArgument, "the H.264 decoder is not ready");
  return d.pop_ready();
}

Result<void> H264Decoder::flush() {
  Impl& d = *impl_;
  if (!d.configured || d.failed) return fail(Status::InvalidArgument, "the H.264 decoder is not ready");
  if (d.flushed) return {};
  if (Result<void> drained = d.drain(); !drained) {
    d.failed = true;
    return drained;
  }
  d.flushed = true;
  return {};
}

std::vector<std::string> H264Decoder::take_warnings() { return std::exchange(impl_->warnings, {}); }

}  // namespace cvt::h264
