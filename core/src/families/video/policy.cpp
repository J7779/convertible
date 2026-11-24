// Convertible core - video family: container codec policies and the muxer factory.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/policy.hpp"

#include <algorithm>
#include <format>

#include "codec/ids.hpp"
#include "families/video/common.hpp"
#include "families/video/containers.hpp"
#include "util/text.hpp"

namespace cvt::video {

namespace {

namespace id = codec::id;

using List = std::vector<std::string_view>;

const List mp4_video = {id::h264, id::hevc, id::av1, id::vp9, id::mpeg4, id::mpeg2video, id::mpeg1video, id::mjpeg, id::vp8};
const List mp4_audio = {id::aac, id::mp3, id::mp2, id::opus, id::flac, id::alac, id::ac3};
const List mov_video = {id::h264,       id::hevc,  id::av1,    id::vp9,   id::mpeg4,   id::mpeg2video,
                        id::mpeg1video, id::mjpeg, id::prores, id::png,   id::h263,    id::qtrle,
                        id::rpza,       id::smc,   id::cinepak, id::uyvy, id::yuy2,    id::v210,
                        id::r210};
const List mov_audio = {id::aac,      id::mp3,       id::mp2,       id::opus,     id::flac,     id::alac,     id::ac3,
                        id::pcm_s16le, id::pcm_s16be, id::pcm_u8,   id::pcm_alaw, id::pcm_mulaw, id::amr_nb, id::amr_wb};
const List mkv_video = {id::h264,       id::hevc,       id::av1,    id::vp8,   id::vp9,    id::mpeg4,
                        id::mpeg1video, id::mpeg2video, id::theora, id::mjpeg, id::prores, id::rawvideo,
                        id::ffv1,       id::huffyuv,    id::utvideo};
const List mkv_audio = {id::aac,  id::mp3,       id::mp2,       id::opus,      id::vorbis,    id::flac,  id::alac,
                        id::ac3,  cid::eac3,     id::pcm_s16le, id::pcm_s16be, id::pcm_s24le, id::pcm_f32le, id::pcm_u8};
const List avi_video = {id::mjpeg, id::mpeg4,   id::h264,     id::hevc,    id::rawvideo, id::mpeg1video, id::mpeg2video,
                        id::vp8,   id::h263,    id::huffyuv,  id::ffv1,    id::utvideo,  id::cinepak,    id::msvideo1,
                        id::msrle, id::uyvy,    id::yuy2,     id::v210};
const List avi_audio = {id::pcm_s16le, id::pcm_u8,  id::pcm_s24le, id::pcm_f32le,  id::mp3,       id::mp2,
                        id::aac,       id::ac3,     id::pcm_alaw,  id::pcm_mulaw, id::adpcm_ima, id::adpcm_ms};

const std::vector<ContainerPolicy>& table() {
  static const std::vector<ContainerPolicy> policies = [] {
    std::vector<ContainerPolicy> p;
    const auto add = [&p](std::string_view fid, List vs, List as, List vp, List ap, bool rotation, bool cfr) {
      ContainerPolicy c;
      c.format_id = fid;
      c.video_storable = std::move(vs);
      c.audio_storable = std::move(as);
      c.video_preferred = std::move(vp);
      c.audio_preferred = std::move(ap);
      c.stores_rotation = rotation;
      c.constant_frame_rate = cfr;
      p.push_back(std::move(c));
    };
    const List mp4_vp = {id::h264, id::hevc, id::av1, id::mpeg4};
    const List mp4_ap = {id::aac, id::mp3, id::opus, id::alac};
    add("mp4", mp4_video, mp4_audio, mp4_vp, mp4_ap, true, false);
    add("m4v", mp4_video, mp4_audio, mp4_vp, mp4_ap, true, false);
    add("mov", mov_video, mov_audio, {id::h264, id::hevc, id::av1, id::mpeg4, id::prores, id::mjpeg},
        {id::aac, id::mp3, id::alac, id::pcm_s16le}, true, false);
    add("3gp", {id::h264, id::h263, id::mpeg4, id::hevc}, {id::aac, id::amr_nb, id::amr_wb},
        {id::h264, id::h263, id::mpeg4}, {id::aac, id::amr_nb}, true, false);
    add("3g2", {id::h264, id::h263, id::mpeg4, id::hevc}, {id::aac, id::amr_nb, id::amr_wb},
        {id::h264, id::h263, id::mpeg4}, {id::aac, id::amr_nb}, true, false);
    add("m4a", {}, {id::aac, id::alac, id::mp3, id::opus, id::flac, id::ac3}, {}, {id::aac, id::alac}, false, false);
    add("m4b", {}, {id::aac, id::alac, id::mp3, id::opus, id::flac, id::ac3}, {}, {id::aac, id::alac}, false, false);
    add("mkv", mkv_video, mkv_audio,
        {id::h264, id::hevc, id::vp9, id::av1, id::vp8, id::mpeg4, id::theora, id::mjpeg, id::rawvideo},
        {id::aac, id::opus, id::vorbis, id::mp3, id::flac, id::pcm_s16le}, true, false);
    add("mka", {}, mkv_audio, {}, {id::opus, id::vorbis, id::aac, id::flac, id::mp3, id::pcm_s16le}, false, false);
    add("webm", {id::vp9, id::vp8, id::av1}, {id::opus, id::vorbis}, {id::vp9, id::vp8, id::av1}, {id::opus, id::vorbis},
        true, false);
    add("avi", avi_video, avi_audio, {id::mjpeg, id::mpeg4, id::h264, id::rawvideo}, {id::pcm_s16le, id::mp3}, false, true);
    add("flv", {id::h264, cid::flv1}, {id::aac, id::mp3, id::pcm_s16le, id::pcm_alaw, id::pcm_mulaw, id::speex},
        {id::h264, cid::flv1}, {id::aac, id::mp3}, false, false);
    add("mpegts", {id::h264, id::hevc, id::mpeg2video, id::mpeg1video, id::mpeg4}, {id::aac, id::mp2, id::mp3, id::ac3},
        {id::h264, id::hevc, id::mpeg2video}, {id::aac, id::mp2, id::mp3, id::ac3}, false, false);
    add("ogv", {id::theora}, {id::vorbis, id::opus, id::flac}, {id::theora}, {id::vorbis, id::opus, id::flac}, false, false);
    add("ogg", {}, {id::vorbis, id::opus, id::flac}, {}, {id::vorbis, id::opus, id::flac}, false, false);
    add("opus", {}, {id::opus}, {}, {id::opus}, false, false);
    add("ivf", {id::vp9, id::vp8, id::av1}, {}, {id::vp9, id::vp8, id::av1}, {}, false, true);
    add("y4m", {id::rawvideo}, {}, {id::rawvideo}, {}, false, true);
    add("h264", {id::h264}, {}, {id::h264}, {}, false, true);
    add("hevc", {id::hevc}, {}, {id::hevc}, {}, false, true);
    add("obu", {id::av1}, {}, {id::av1}, {}, false, true);
    add("m2v", {id::mpeg2video, id::mpeg1video}, {}, {id::mpeg2video, id::mpeg1video}, {}, false, true);
    // Audio-only extraction targets (Video -> audio file without decoding when possible).
    add("mp3", {}, {id::mp3}, {}, {id::mp3}, false, false);
    add("aac", {}, {id::aac}, {}, {id::aac}, false, false);
    add("flac", {}, {id::flac}, {}, {id::flac}, false, false);
    add("wav", {}, {id::pcm_s16le, id::pcm_u8, id::pcm_s24le, id::pcm_f32le, id::pcm_alaw, id::pcm_mulaw}, {},
        {id::pcm_s16le}, false, false);
    for (ContainerPolicy& c : p) {
      c.multiple_audio = c.format_id != "flv" && c.format_id != "ivf" && c.format_id != "y4m" && c.format_id != "mp3" &&
                         c.format_id != "aac" && c.format_id != "flac" && c.format_id != "wav" && c.format_id != "opus";
    }
    return p;
  }();
  return policies;
}

}  // namespace

const ContainerPolicy* policy_for(std::string_view format_id) {
  for (const ContainerPolicy& p : table()) {
    if (p.format_id == format_id) return &p;
  }
  return nullptr;
}

std::span<const ContainerPolicy> all_policies() { return table(); }

bool container_accepts(std::string_view format_id, ir::TrackType type, std::string_view codec) {
  const ContainerPolicy* p = policy_for(format_id);
  if (p == nullptr) return false;
  const List& list = type == ir::TrackType::Video ? p->video_storable : type == ir::TrackType::Audio ? p->audio_storable : List{};
  return std::ranges::find(list, codec) != list.end();
}

Result<CodecChoice> choose_encoder(const ContainerPolicy& policy, ir::TrackType type, std::string_view requested,
                                   const codec::Registry& media) {
  const bool video = type == ir::TrackType::Video;
  const List& storable = video ? policy.video_storable : policy.audio_storable;
  const List& preferred = video ? policy.video_preferred : policy.audio_preferred;
  const auto has = [&](std::string_view c) { return video ? media.has_video_encoder(c) : media.has_audio_encoder(c); };
  const char* kind = video ? "video" : "audio";
  if (!requested.empty() && requested != "auto" && requested != "copy") {
    if (std::ranges::find(storable, requested) == storable.end()) {
      std::vector<std::string_view> names(storable.begin(), storable.end());
      return fail(Status::InvalidArgument, "{} cannot hold {} {}; it accepts: {}", policy.format_id, kind, requested,
                  util::join(names, ", "));
    }
    if (!has(requested)) {
      return fail(Status::Encode, "no {} encoder for '{}' is available in this build", kind, requested);
    }
    return CodecChoice{std::string(requested)};
  }
  for (const std::string_view c : preferred) {
    if (has(c)) return CodecChoice{std::string(c)};
  }
  std::vector<std::string_view> wanted(preferred.begin(), preferred.end());
  std::vector<std::string> available;
  for (const codec::CodecListing& l : media.list()) {
    if (l.role == (video ? codec::CodecRole::VideoEncoder : codec::CodecRole::AudioEncoder)) available.push_back(l.id);
  }
  return fail(Status::Encode, "{} needs a {} encoder for one of: {}; none of them is available in this build ({} encoders: {})",
              policy.format_id, kind, util::join(wanted, ", "), kind, available.empty() ? std::string("none") : util::join(available, ", "));
}

std::vector<std::string> video_codec_choices() {
  std::vector<std::string> out = {"auto", "copy"};
  for (const ContainerPolicy& p : table()) {
    for (const std::string_view c : p.video_storable) {
      if (std::ranges::find(out, c) == out.end()) out.emplace_back(c);
    }
  }
  return out;
}

std::vector<std::string> audio_codec_choices() {
  std::vector<std::string> out = {"auto", "copy"};
  for (const ContainerPolicy& p : table()) {
    for (const std::string_view c : p.audio_storable) {
      if (std::ranges::find(out, c) == out.end()) out.emplace_back(c);
    }
  }
  return out;
}

std::unique_ptr<Muxer> make_muxer(std::string_view format_id) {
  if (format_id == "mp4" || format_id == "m4v" || format_id == "mov" || format_id == "3gp" || format_id == "3g2" ||
      format_id == "m4a" || format_id == "m4b") {
    return make_mp4_muxer(format_id);
  }
  if (format_id == "mkv" || format_id == "mka") return make_mkv_muxer(false);
  if (format_id == "webm") return make_mkv_muxer(true);
  if (format_id == "avi") return make_avi_muxer();
  if (format_id == "flv") return make_flv_muxer();
  if (format_id == "mpegts") return make_ts_muxer();
  if (format_id == "ogv" || format_id == "ogg" || format_id == "opus") return make_ogg_muxer();
  if (format_id == "ivf") return make_ivf_muxer();
  if (format_id == "y4m") return make_y4m_muxer();
  if (format_id == "h264" || format_id == "hevc" || format_id == "obu" || format_id == "m2v" || format_id == "mp3" ||
      format_id == "aac" || format_id == "flac" || format_id == "wav") {
    return make_raw_muxer(format_id);
  }
  return nullptr;
}

}  // namespace cvt::video
