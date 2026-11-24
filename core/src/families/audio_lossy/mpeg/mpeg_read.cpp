// Convertible core - audio_lossy family: reading MPEG audio files: tags
// (ID3v2.2-2.4, ID3v1/1.1, APEv2, Lyrics3v2 skipped), frame decoding and
// gapless trimming from the LAME/Info tag; plus the packet decoder that the
// codec registry hands to containers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "families/audio_lossless/common.hpp"
#include "families/audio_lossless/tags.hpp"
#include "families/audio_lossy/mpeg/mpeg_audio.hpp"
#include "families/audio_lossy/mpeg/mpeg_files.hpp"
#include "util/text.hpp"

namespace cvt::lossy::mpeg {

namespace {

constexpr std::array<std::string_view, 80> id3v1_genres = {
    "Blues",       "Classic Rock",     "Country",        "Dance",           "Disco",        "Funk",
    "Grunge",      "Hip-Hop",          "Jazz",           "Metal",           "New Age",      "Oldies",
    "Other",       "Pop",              "R&B",            "Rap",             "Reggae",       "Rock",
    "Techno",      "Industrial",       "Alternative",    "Ska",             "Death Metal",  "Pranks",
    "Soundtrack",  "Euro-Techno",      "Ambient",        "Trip-Hop",        "Vocal",        "Jazz+Funk",
    "Fusion",      "Trance",           "Classical",      "Instrumental",    "Acid",         "House",
    "Game",        "Sound Clip",       "Gospel",         "Noise",           "AlternRock",   "Bass",
    "Soul",        "Punk",             "Space",          "Meditative",      "Instrumental Pop",
    "Instrumental Rock", "Ethnic",     "Gothic",         "Darkwave",        "Techno-Industrial",
    "Electronic",  "Pop-Folk",         "Eurodance",      "Dream",           "Southern Rock", "Comedy",
    "Cult",        "Gangsta",          "Top 40",         "Christian Rap",   "Pop/Funk",     "Jungle",
    "Native American", "Cabaret",      "New Wave",       "Psychadelic",     "Rave",         "Showtunes",
    "Trailer",     "Lo-Fi",            "Tribal",         "Acid Punk",       "Acid Jazz",    "Polka",
    "Retro",       "Musical",          "Rock & Roll",    "Hard Rock",
};

std::size_t syncsafe(std::span<const std::uint8_t> b) {
  return (std::size_t{b[0]} << 21) | (std::size_t{b[1]} << 14) | (std::size_t{b[2]} << 7) | b[3];
}

/// ID3v1 text: Latin-1, NUL/space padded.
std::string id3v1_text(std::span<const std::uint8_t> field) {
  std::size_t n = field.size();
  while (n > 0 && (field[n - 1] == 0 || field[n - 1] == ' ')) --n;
  for (std::size_t i = 0; i < n; ++i) {
    if (field[i] == 0) {
      n = i;
      break;
    }
  }
  return util::decode_text(as_bytes(field.first(n)), util::TextEncoding::Latin1).text;
}

/// "(26)" or "(26)Ambient" (ID3v2.3 genre references) -> "Ambient".
void normalise_genre(std::map<std::string, std::string>& tags) {
  const auto it = tags.find("genre");
  if (it == tags.end()) return;
  std::string& g = it->second;
  if (g.size() >= 3 && g.front() == '(') {
    const std::size_t close = g.find(')');
    if (close != std::string::npos) {
      const auto number = util::parse_int(std::string_view(g).substr(1, close - 1));
      std::string rest = g.substr(close + 1);
      if (!rest.empty()) {
        g = rest;
      } else if (number && *number >= 0 && *number < static_cast<std::int64_t>(id3v1_genres.size())) {
        g = std::string(id3v1_genres[static_cast<std::size_t>(*number)]);
      }
    }
  } else if (const auto number = util::parse_int(g); number && *number >= 0 &&
                                                       *number < static_cast<std::int64_t>(id3v1_genres.size())) {
    g = std::string(id3v1_genres[static_cast<std::size_t>(*number)]);
  }
}

}  // namespace

TagScan scan_tags(ByteView data) {
  TagScan scan;
  const auto d = as_u8(data);
  std::size_t pos = 0;
  // Leading ID3v2 tags (there may be more than one).
  while (d.size() - pos >= 10 && std::memcmp(d.data() + pos, "ID3", 3) == 0 && d[pos + 3] < 5) {
    const auto size_bytes = d.subspan(pos + 6, 4);
    if ((size_bytes[0] | size_bytes[1] | size_bytes[2] | size_bytes[3]) & 0x80) break;
    std::size_t total = 10 + syncsafe(size_bytes) + ((d[pos + 5] & 0x10) != 0 ? 10 : 0);
    total = std::min(total, d.size() - pos);
    audio::tags::parse_id3v2(data.subspan(pos, total), scan.tags);
    pos += total;
  }
  scan.start = pos;
  std::size_t end = d.size();
  std::map<std::string, std::string> trailing;
  // ID3v1 at the very end.
  bool v1 = false;
  if (end - pos >= 128 && std::memcmp(d.data() + end - 128, "TAG", 3) == 0) {
    v1 = true;
    const auto t = d.subspan(end - 128, 128);
    audio::tags::put(trailing, "title", id3v1_text(t.subspan(3, 30)));
    audio::tags::put(trailing, "artist", id3v1_text(t.subspan(33, 30)));
    audio::tags::put(trailing, "album", id3v1_text(t.subspan(63, 30)));
    audio::tags::put(trailing, "date", id3v1_text(t.subspan(93, 4)));
    if (t[125] == 0 && t[126] != 0) {
      audio::tags::put(trailing, "comment", id3v1_text(t.subspan(97, 28)));
      audio::tags::put(trailing, "track", std::to_string(t[126]));
    } else {
      audio::tags::put(trailing, "comment", id3v1_text(t.subspan(97, 30)));
    }
    if (t[127] < id3v1_genres.size()) audio::tags::put(trailing, "genre", id3v1_genres[t[127]]);
    end -= 128;
  }
  // APEv2 (the parser looks behind an ID3v1 tag itself).
  std::map<std::string, std::string> ape;
  const std::size_t ape_size = audio::tags::parse_apev2(data.first(end + (v1 ? 128 : 0)), ape);
  if (ape_size > 0 && ape_size <= end - pos) end -= ape_size;
  // Lyrics3v2: "...<6-digit size>LYRICS200" before the ID3v1 tag.
  if (end - pos >= 15 && std::memcmp(d.data() + end - 9, "LYRICS200", 9) == 0) {
    const auto size = util::parse_int(as_chars(data.subspan(end - 15, 6)));
    if (size && *size >= 0 && static_cast<std::uint64_t>(*size) + 15 <= end - pos) {
      end -= static_cast<std::size_t>(*size) + 15;
    }
  }
  scan.end = end;
  for (const auto& [k, v] : ape) audio::tags::put(scan.tags, k, v);
  for (const auto& [k, v] : trailing) audio::tags::put(scan.tags, k, v);
  normalise_genre(scan.tags);
  return scan;
}

Result<ir::Audio> read_mpeg_file(ByteView data, const Context& ctx) {
  TagScan tags = scan_tags(data);
  CVT_TRY_ASSIGN(ScanResult scan, scan_stream(data, tags.start, tags.end, ctx));
  const FrameHeader& first = scan.first;
  std::size_t index = 0;
  StreamInfo info;
  if (const auto tag = parse_info_frame(data.subspan(scan.frames[0].offset, scan.frames[0].size), first)) {
    info = *tag;
    index = 1;
  }
  const std::uint64_t audio_frames = scan.frames.size() - index;
  const std::uint64_t spf = first.samples();
  CVT_TRY(audio::check_sample_budget(ctx, audio_frames * spf * static_cast<std::uint64_t>(first.channels())));
  ir::Audio audio;
  audio.sample_rate = first.sample_rate;
  audio.channels = static_cast<std::uint16_t>(first.channels());
  audio.samples.reserve(static_cast<std::size_t>(audio_frames * spf * audio.channels));
  FrameDecoder decoder;
  std::size_t errors = 0;
  for (std::size_t i = index; i < scan.frames.size(); ++i) {
    if ((i & 63) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(scan.frames.size()), "Decoding"));
    }
    const FrameRef& f = scan.frames[i];
    const std::size_t before = audio.samples.size();
    if (auto r = decoder.decode(data.subspan(f.offset, f.size), audio.samples); !r) {
      ++errors;
      audio.samples.resize(before + static_cast<std::size_t>(spf) * audio.channels, 0.0f);
      decoder.reset();
    }
  }
  if (errors * 2 > audio_frames) return fail(Status::Decode, "the MPEG audio stream is too damaged to decode");
  if (errors > 0 || scan.resynced) ctx.warn("Damaged MPEG audio frames were skipped or replaced by silence.");
  // Gapless playback: drop the encoder delay (plus the 529-sample decoder
  // delay of Layer III) and the final padding recorded by the encoder.
  if (info.has_lame && first.layer == 3) {
    const std::uint64_t total = audio.frame_count();
    const std::uint64_t skip = std::min<std::uint64_t>(total, std::uint64_t{info.encoder_delay} + 529);
    const std::uint64_t trimmed = audio_frames * spf;
    const std::uint64_t pad = std::uint64_t{info.encoder_delay} + info.encoder_padding;
    const std::uint64_t keep = trimmed > pad ? std::min(trimmed - pad, total - skip) : 0;
    audio.samples.erase(audio.samples.begin(), audio.samples.begin() + static_cast<std::ptrdiff_t>(skip * audio.channels));
    audio.samples.resize(static_cast<std::size_t>(keep * audio.channels));
  }
  audio.tags = std::move(tags.tags);
  return audio;
}

// =========================================================================== registry decoder

namespace {

class PacketDecoder final : public codec::AudioDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    sample_rate_ = params.sample_rate;
    channels_ = params.channels;
    return {};
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    // A packet normally holds one frame; tolerate several (or a raw stream).
    const ByteView data(packet.bytes);
    std::size_t pos = 0;
    while (data.size() - pos >= 4) {
      const auto h = parse_header(header_word(data, pos));
      if (!h) {
        ++pos;
        continue;
      }
      std::size_t size = h->frame_bytes(h->bitrate_kbps);
      if (size == 0) size = data.size() - pos;  // free format: the whole packet
      if (size > data.size() - pos) size = data.size() - pos;
      if (first_) {
        first_ = false;
        if (const auto info = parse_info_frame(data.subspan(pos, size), *h); info && info->has_tag) {
          pos += size;
          continue;
        }
      }
      const std::size_t before = pending_.size();
      if (auto r = decoder_.decode(data.subspan(pos, size), pending_); !r) {
        pending_.resize(before + std::size_t{h->samples()} * static_cast<std::size_t>(h->channels()), 0.0f);
        decoder_.reset();
      }
      sample_rate_ = decoder_.sample_rate();
      channels_ = static_cast<std::uint16_t>(decoder_.channels());
      pos += size;
    }
    return {};
  }
  Result<std::optional<codec::AudioChunk>> receive_chunk() override {
    if (pending_.empty()) return std::optional<codec::AudioChunk>{};
    codec::AudioChunk chunk;
    chunk.samples = std::move(pending_);
    pending_.clear();
    return std::optional<codec::AudioChunk>(std::move(chunk));
  }
  Result<void> flush() override { return {}; }
  [[nodiscard]] std::uint32_t sample_rate() const override { return sample_rate_; }
  [[nodiscard]] std::uint16_t channels() const override { return channels_; }

 private:
  FrameDecoder decoder_;
  std::vector<float> pending_;
  std::uint32_t sample_rate_ = 0;
  std::uint16_t channels_ = 0;
  bool first_ = true;
};

}  // namespace

std::unique_ptr<codec::AudioDecoder> make_mpeg_decoder() { return std::make_unique<PacketDecoder>(); }

}  // namespace cvt::lossy::mpeg
