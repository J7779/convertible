// Convertible core - video family: container readers and writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <string_view>

#include "families/video/common.hpp"
#include "families/video/mux.hpp"

namespace cvt::video {

// Demuxers: parse the whole container index (never the payloads).
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_mp4(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_mkv(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_avi(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_flv(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_ts(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_ps(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_ogg(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_asf(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_ivf(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_y4m(ByteView input, const ReadSettings& settings);
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_rm(ByteView input, const ReadSettings& settings);
/// Raw elementary streams: `codec` is h264, hevc, av1 (low-overhead OBUs) or mpeg2video/mpeg1video.
[[nodiscard]] Result<std::shared_ptr<Demuxed>> demux_raw(ByteView input, const ReadSettings& settings,
                                                         std::string_view codec, double fps);

/// A timestamp attached to a byte position of an elementary stream (from a PES header).
struct EsMarker {
  std::uint64_t offset = 0;
  std::optional<std::int64_t> pts;
  std::optional<std::int64_t> dts;
};
/// A reassembled elementary stream: input spans in order, and their total size.
struct EsStream {
  std::vector<Span> spans;
  std::uint64_t size = 0;
  void add(std::uint64_t offset, std::uint64_t length);
  /// The spans covering [begin, end) of the stream.
  [[nodiscard]] std::vector<Span> slice(std::uint64_t begin, std::uint64_t end) const;
  /// Up to count bytes starting at position, copied.
  [[nodiscard]] Bytes read(ByteView input, std::uint64_t position, std::size_t count) const;
};
/// Split an MPEG-1/2 video elementary stream into pictures and add them to
/// the track (90 kHz timestamps; pictures without a marker are interpolated).
void split_mpeg12_video(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<EsMarker>& markers,
                        double fallback_fps);
/// Split an Annex-B H.264/HEVC stream into access units (same timing rules).
/// Give an H.264 track without presentation times (raw stream, AVI) the times
/// implied by its picture order counts; leaves the track alone when unsure.
void derive_h264_pts(Demuxed& demuxed, TrackInfo& track);
void split_annexb_video(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<EsMarker>& markers,
                        double fallback_fps);

/// Split a framed audio elementary stream (ADTS AAC, MPEG audio, AC-3) into
/// frames; frames may straddle PES boundaries. Other codecs: one packet per marker.
void split_audio_frames(Demuxed& demuxed, TrackInfo& track, const EsStream& es, const std::vector<EsMarker>& markers);

// Muxers.
[[nodiscard]] std::unique_ptr<Muxer> make_mp4_muxer(std::string_view format_id);
[[nodiscard]] std::unique_ptr<Muxer> make_mkv_muxer(bool webm);
[[nodiscard]] std::unique_ptr<Muxer> make_avi_muxer();
[[nodiscard]] std::unique_ptr<Muxer> make_flv_muxer();
[[nodiscard]] std::unique_ptr<Muxer> make_ts_muxer();
[[nodiscard]] std::unique_ptr<Muxer> make_ogg_muxer();
[[nodiscard]] std::unique_ptr<Muxer> make_ivf_muxer();
[[nodiscard]] std::unique_ptr<Muxer> make_y4m_muxer();
[[nodiscard]] std::unique_ptr<Muxer> make_raw_muxer(std::string_view codec);

// Tag tables shared by readers and writers.
/// Canonical codec for an AVI/ASF/Matroska-VFW video FourCC; empty when unknown.
[[nodiscard]] std::string_view codec_for_bitmap_fourcc(std::uint32_t fourcc) noexcept;
/// Canonical codec for a WAVEFORMATEX format tag; empty when unknown.
[[nodiscard]] std::string_view codec_for_wave_tag(std::uint16_t tag, std::uint16_t bits) noexcept;
/// Inverse of the above, for writers; 0 when the codec has no tag.
[[nodiscard]] std::uint32_t bitmap_fourcc_for_codec(std::string_view codec) noexcept;
[[nodiscard]] std::uint16_t wave_tag_for_codec(std::string_view codec) noexcept;

/// Frame size and samples of an AC-3 sync frame; nullopt when not one.
struct Ac3Header {
  std::size_t frame_size = 0;
  std::uint32_t sample_rate = 0;
  int channels = 0;
  int samples = 1536;
  int fscod = 0;
  int bsid = 8;
  int bsmod = 0;
  int acmod = 0;
  int lfe = 0;
  int bitrate_code = 0;
};
[[nodiscard]] std::optional<Ac3Header> parse_ac3(ByteView data) noexcept;

}  // namespace cvt::video
