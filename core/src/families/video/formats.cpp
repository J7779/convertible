// Convertible core - video family: format descriptions, readers and writers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

#include "engine/registry.hpp"
#include "families/video/containers.hpp"
#include "families/video/pipeline.hpp"
#include "families/video/policy.hpp"
#include "families/video/video.hpp"
#include "util/text.hpp"

namespace cvt::video {

namespace {

using Demux = std::function<Result<std::shared_ptr<Demuxed>>(ByteView, const ReadSettings&, const Options&)>;

std::string_view extension_of(std::string_view filename) {
  const std::size_t dot = filename.rfind('.');
  return dot == std::string_view::npos ? std::string_view{} : filename.substr(dot + 1);
}

bool has_extension(std::string_view filename, std::string_view ext) { return util::iequals(extension_of(filename), ext); }

std::string_view head(std::span<const std::byte> data, std::size_t n) { return as_chars(data.first(std::min(n, data.size()))); }

std::uint8_t byte_at(std::span<const std::byte> data, std::size_t i) {
  return i < data.size() ? std::to_integer<std::uint8_t>(data[i]) : 0;
}

int sniff_ts(std::span<const std::byte> data, std::string_view) {
  for (const std::size_t size : {std::size_t{188}, std::size_t{192}, std::size_t{204}}) {
    const std::size_t lead = size == 192 ? 4 : 0;
    int hits = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const std::size_t at = lead + k * size;
      if (at >= data.size()) break;
      if (byte_at(data, at) != 0x47) {
        hits = -1;
        break;
      }
      ++hits;
    }
    if (hits >= 2 || (hits == 1 && data.size() <= size + lead)) return hits >= 3 ? 92 : 75;
  }
  return 0;
}

int sniff_annexb(std::span<const std::byte> data, std::string_view filename, bool hevc) {
  std::size_t at = 0;
  if (head(data, 4) == std::string_view("\0\0\0\1", 4)) {
    at = 4;
  } else if (head(data, 3) == std::string_view("\0\0\1", 3)) {
    at = 3;
  } else {
    return 0;
  }
  const std::uint8_t b = byte_at(data, at);
  int score = 0;
  if (!hevc && (b & 0x80) == 0 && ((b & 0x1F) == 7 || (b & 0x1F) == 9)) score = 70;
  if (hevc && (b == 0x40 || b == 0x46 || b == 0x42) && byte_at(data, at + 1) == 0x01) score = 70;
  if (score > 0 && (hevc ? (has_extension(filename, "h265") || has_extension(filename, "hevc") || has_extension(filename, "265"))
                         : (has_extension(filename, "h264") || has_extension(filename, "264")))) {
    score = 90;
  }
  return score;
}

int sniff_obu(std::span<const std::byte> data, std::string_view filename) {
  // Temporal delimiter (size 0) followed by a sequence header OBU.
  if (byte_at(data, 0) == 0x12 && byte_at(data, 1) == 0x00 && (byte_at(data, 2) & 0x78) == 0x08) {
    return has_extension(filename, "obu") ? 92 : 70;
  }
  return 0;
}

int sniff_m2v(std::span<const std::byte> data, std::string_view filename) {
  if (head(data, 4) == std::string_view("\0\0\1\xB3", 4)) return has_extension(filename, "mpg") ? 40 : 90;
  return 0;
}

int sniff_vob(std::span<const std::byte> data, std::string_view filename) {
  if (head(data, 4) == std::string_view("\0\0\1\xBA", 4) && has_extension(filename, "vob")) return 96;
  return 0;
}

bool asf_has_video(std::span<const std::byte> data) {
  // Stream type GUID for video: BC19EFC0-5B4D-11CF-A8FD-00805F5C442B.
  static constexpr unsigned char video_guid[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                                   0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
  const std::string_view h = head(data, 64 * 1024);
  return h.find(std::string_view(reinterpret_cast<const char*>(video_guid), 16)) != std::string_view::npos;
}

int sniff_asf(std::span<const std::byte> data, std::string_view, bool want_video) {
  static constexpr unsigned char header_guid[8] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11};
  if (head(data, 8) != std::string_view(reinterpret_cast<const char*>(header_guid), 8)) return 0;
  return asf_has_video(data) == want_video ? 96 : 0;
}

int sniff_ogv(std::span<const std::byte> data, std::string_view) {
  if (head(data, 4) != "OggS") return 0;
  return head(data, 64 * 1024).find("\x80theora") != std::string_view::npos ? 96 : 0;
}

ReadSettings settings_of(const ReadContext& ctx) { return ReadSettings{ctx.limits, ctx.progress, ctx.warnings}; }

void add_container(Registry& registry, FormatInfo info, Demux demux, bool writable, bool raw_stream = false,
                   int read_cost = 10) {
  const std::string id = info.id;
  registry.add_format(std::move(info));
  Codec reader;
  reader.format_id = id;
  reader.kind = Kind::Video;
  reader.read_cost = read_cost;
  reader.read_options = container_read_options(raw_stream);
  reader.read = [demux](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(std::shared_ptr<Demuxed> demuxed, demux(ctx.input, settings_of(ctx), ctx.options));
    demuxed->format_id = ctx.format_id;
    CVT_TRY_ASSIGN(ir::Video video, make_video(std::move(demuxed), ctx));
    return AnyIR(std::move(video));
  };
  registry.add_codec(std::move(reader));
  if (writable) {
    Codec writer;
    writer.format_id = id;
    writer.kind = Kind::Video;
    writer.lossy = true;
    writer.write_cost = 10;
    writer.write_options = container_write_options(id);
    writer.write = [](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      return write_container(value.get<ir::Video>(), ctx, ctx.format_id);
    };
    registry.add_codec(std::move(writer));
  }
}

FormatInfo format(std::string id, std::string name, std::string description, Category category,
                  std::vector<std::string> extensions, std::string mime, int popularity) {
  FormatInfo f;
  f.id = std::move(id);
  f.name = std::move(name);
  f.description = std::move(description);
  f.category = category;
  f.extensions = std::move(extensions);
  f.mime = std::move(mime);
  f.popularity = popularity;
  return f;
}

Demux simple(DemuxFunction fn) {
  return [fn](ByteView input, const ReadSettings& s, const Options&) { return fn(input, s); };
}

Demux raw(std::string_view codec) {
  const std::string c(codec);
  return [c](ByteView input, const ReadSettings& s, const Options& o) {
    return demux_raw(input, s, c, o.get_float("raw_fps", 25.0, 1.0, 1000.0));
  };
}

}  // namespace

int sniff_iso(std::span<const std::byte> data, std::string_view, std::string_view format_id) {
  if (data.size() < 12) return 0;
  const std::string_view type = head(data, 12).substr(4, 4);
  std::string detected;
  if (type == "ftyp") {
    const std::string_view brand = head(data, 12).substr(8, 4);
    if (brand == "qt  ") {
      detected = "mov";
    } else if (brand == "M4V " || brand == "M4VH" || brand == "M4VP") {
      detected = "m4v";
    } else if (brand == "M4A ") {
      detected = "m4a";
    } else if (brand == "M4B ") {
      detected = "m4b";
    } else if (brand.starts_with("3g2")) {
      detected = "3g2";
    } else if (brand.starts_with("3gp") || brand.starts_with("3ge") || brand.starts_with("3gg")) {
      detected = "3gp";
    } else if (brand == "avif" || brand == "avis" || brand == "heic" || brand == "heix" || brand == "mif1" ||
               brand == "msf1" || brand == "crx ") {
      return 0;  // still-image formats built on the same boxes
    } else {
      detected = "mp4";
    }
    return detected == format_id ? 96 : 0;
  }
  if (type == "moov" || type == "mdat" || type == "wide" || type == "free" || type == "skip" || type == "pnot") {
    return format_id == "mov" ? 85 : 0;
  }
  return 0;
}

int sniff_ebml(std::span<const std::byte> data, std::string_view filename, std::string_view format_id) {
  if (head(data, 4) != "\x1A\x45\xDF\xA3") return 0;
  const std::string_view h = head(data, 64);
  const bool webm = h.find("webm") != std::string_view::npos;
  const bool matroska = h.find("matroska") != std::string_view::npos;
  if (format_id == "webm") return webm ? 96 : 0;
  if (format_id == "mka") return matroska && has_extension(filename, "mka") ? 97 : 0;
  return matroska ? 95 : 0;
}

void register_formats(Registry& registry) {
  using C = Category;
  auto iso = [](std::string id) {
    return [id](std::span<const std::byte> d, std::string_view f) { return sniff_iso(d, f, id); };
  };
  auto ebml = [](std::string id) {
    return [id](std::span<const std::byte> d, std::string_view f) { return sniff_ebml(d, f, id); };
  };

  FormatInfo f = format("mp4", "MPEG-4 video", "The most widely supported video file (H.264/HEVC/AV1 with AAC).", C::Video,
                        {"mp4"}, "video/mp4", 95);
  f.sniff = iso("mp4");
  add_container(registry, std::move(f), simple(demux_mp4), true);
  f = format("m4v", "MPEG-4 video (iTunes)", "MPEG-4 video as used by Apple devices.", C::Video, {"m4v"}, "video/x-m4v", 40);
  f.signatures = {Signature::at(4, "'ftypM4V '")};
  f.sniff = iso("m4v");
  add_container(registry, std::move(f), simple(demux_mp4), true);
  f = format("mov", "QuickTime movie", "Apple QuickTime movie.", C::Video, {"mov", "qt"}, "video/quicktime", 80);
  f.sniff = iso("mov");
  add_container(registry, std::move(f), simple(demux_mp4), true);
  f = format("3gp", "3GPP video", "Mobile phone video (3GPP).", C::Video, {"3gp"}, "video/3gpp", 30);
  f.sniff = iso("3gp");
  add_container(registry, std::move(f), simple(demux_mp4), true);
  f = format("3g2", "3GPP2 video", "Mobile phone video (3GPP2).", C::Video, {"3g2"}, "video/3gpp2", 15);
  f.signatures = {Signature::at(4, "'ftyp3g2'")};
  f.sniff = iso("3g2");
  add_container(registry, std::move(f), simple(demux_mp4), true);
  f = format("m4a", "MPEG-4 audio", "AAC or ALAC audio in an MPEG-4 file.", C::Audio, {"m4a"}, "audio/mp4", 80);
  f.sniff = iso("m4a");
  add_container(registry, std::move(f), simple(demux_mp4), true);
  f = format("m4b", "MPEG-4 audiobook", "MPEG-4 audio used for audiobooks.", C::Audio, {"m4b"}, "audio/mp4", 30);
  f.sniff = iso("m4b");
  add_container(registry, std::move(f), simple(demux_mp4), true);

  f = format("mkv", "Matroska video", "Open container that can hold almost any codec.", C::Video, {"mkv", "mk3d"},
             "video/x-matroska", 80);
  f.sniff = ebml("mkv");
  add_container(registry, std::move(f), simple(demux_mkv), true);
  f = format("mka", "Matroska audio", "Matroska file holding audio only.", C::Audio, {"mka"}, "audio/x-matroska", 25);
  f.sniff = ebml("mka");
  add_container(registry, std::move(f), simple(demux_mkv), true);
  f = format("webm", "WebM video", "Royalty-free web video (VP9/AV1 with Opus).", C::Video, {"webm"}, "video/webm", 80);
  f.sniff = ebml("webm");
  add_container(registry, std::move(f), simple(demux_mkv), true);

  f = format("avi", "AVI video", "Microsoft Audio Video Interleave.", C::Video, {"avi"}, "video/x-msvideo", 65);
  f.signatures = {Signature::at(0, "'RIFF' ?? ?? ?? ?? 'AVI '")};
  add_container(registry, std::move(f), simple(demux_avi), true);
  f = format("flv", "Flash video", "Flash Video, used by older web players and RTMP.", C::Video, {"flv"}, "video/x-flv", 30);
  f.signatures = {Signature::at(0, "'FLV' 01")};
  add_container(registry, std::move(f), simple(demux_flv), true);
  f = format("ts", "MPEG transport stream", "Broadcast and streaming video (MPEG-TS, M2TS).", C::Video, {"ts", "m2ts", "mts"},
             "video/mp2t", 50);
  f.sniff = sniff_ts;
  add_container(registry, std::move(f), simple(demux_ts), true);
  f = format("mpg", "MPEG video", "MPEG-1/MPEG-2 program stream.", C::Video, {"mpg", "mpeg"}, "video/mpeg", 45);
  f.signatures = {Signature::at(0, "00 00 01 BA")};
  add_container(registry, std::move(f), simple(demux_ps), false);
  f = format("vob", "DVD video object", "MPEG-2 program stream from a DVD.", C::Video, {"vob"}, "video/dvd", 25);
  f.sniff = sniff_vob;
  add_container(registry, std::move(f), simple(demux_ps), false);
  f = format("ogv", "Ogg video", "Theora video in an Ogg file.", C::Video, {"ogv"}, "video/ogg", 30);
  f.sniff = sniff_ogv;
  add_container(registry, std::move(f), simple(demux_ogg), true);
  f = format("wmv", "Windows Media video", "Advanced Systems Format (ASF) with video.", C::Video, {"wmv", "asf"},
             "video/x-ms-wmv", 45);
  f.sniff = [](std::span<const std::byte> d, std::string_view n) { return sniff_asf(d, n, true); };
  add_container(registry, std::move(f), simple(demux_asf), false);
  f = format("wma", "Windows Media audio", "Advanced Systems Format (ASF) with audio only.", C::Audio, {"wma"},
             "audio/x-ms-wma", 35);
  f.sniff = [](std::span<const std::byte> d, std::string_view n) { return sniff_asf(d, n, false); };
  add_container(registry, std::move(f), simple(demux_asf), false);
  f = format("ivf", "IVF video", "Simple container for VP8, VP9 and AV1 streams.", C::Video, {"ivf"}, "video/x-ivf", 10);
  f.signatures = {Signature::at(0, "'DKIF' 00 00")};
  add_container(registry, std::move(f), simple(demux_ivf), true);
  f = format("y4m", "YUV4MPEG2 video", "Uncompressed YCbCr frames with a text header.", C::Video, {"y4m"},
             "video/x-yuv4mpeg", 10);
  f.signatures = {Signature::at(0, "'YUV4MPEG2 '")};
  add_container(registry, std::move(f), simple(demux_y4m), true);
  f = format("rm", "RealMedia", "RealNetworks RealMedia file.", C::Video, {"rm", "rmvb"}, "application/vnd.rn-realmedia", 15);
  f.signatures = {Signature::at(0, "'.RMF' 00 00 00")};
  add_container(registry, std::move(f), simple(demux_rm), false);

  f = format("h264", "H.264 elementary stream", "Raw H.264/AVC video (Annex B byte stream).", C::Video, {"h264", "264"},
             "video/h264", 15);
  f.sniff = [](std::span<const std::byte> d, std::string_view n) { return sniff_annexb(d, n, false); };
  add_container(registry, std::move(f), raw("h264"), true, true);
  f = format("hevc", "HEVC elementary stream", "Raw H.265/HEVC video (Annex B byte stream).", C::Video,
             {"h265", "hevc", "265"}, "video/h265", 15);
  f.sniff = [](std::span<const std::byte> d, std::string_view n) { return sniff_annexb(d, n, true); };
  add_container(registry, std::move(f), raw("hevc"), true, true);
  f = format("obu", "AV1 OBU stream", "Raw AV1 video (low-overhead bitstream).", C::Video, {"obu"}, "video/av1", 5);
  f.sniff = sniff_obu;
  add_container(registry, std::move(f), raw("av1"), true, true);
  f = format("m2v", "MPEG video elementary stream", "Raw MPEG-1/MPEG-2 video.", C::Video, {"m2v", "m1v", "mpv"},
             "video/mpeg", 10);
  f.sniff = sniff_m2v;
  add_container(registry, std::move(f), raw("mpeg2video"), true, true);

  // Ogg audio files are owned by the audio families; reading them as a video
  // (stream of packets) lets them be remuxed into Matroska/MP4/WebM unchanged.
  for (const char* id : {"ogg", "opus"}) {
    Codec reader;
    reader.format_id = id;
    reader.kind = Kind::Video;
    reader.read_cost = 30;
    reader.read_options = container_read_options(false);
    reader.read = [](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(std::shared_ptr<Demuxed> demuxed, demux_ogg(ctx.input, settings_of(ctx)));
      demuxed->format_id = ctx.format_id;
      CVT_TRY_ASSIGN(ir::Video video, make_video(std::move(demuxed), ctx));
      return AnyIR(std::move(video));
    };
    registry.add_codec(std::move(reader));
  }
}

}  // namespace cvt::video
