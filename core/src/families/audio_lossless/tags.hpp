// Convertible core - audio_lossless family: tag formats shared by the
// containers (RIFF INFO, ID3v2, Vorbis comments, iTunes item names).
// Tags use the ir::Audio vocabulary: lower-case keys "title", "artist",
// "album", "albumartist", "date", "track", "disc", "genre", "comment",
// "composer", "copyright", "encoder"; values are UTF-8.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "util/bytes.hpp"

namespace cvt::audio::tags {

using TagMap = std::map<std::string, std::string>;

/// Add a tag (trimmed, NULs removed, invalid UTF-8 repaired); empty values
/// are ignored; an existing value is kept.
void put(TagMap& tags, std::string_view key, std::string_view value);

// RIFF LIST/INFO ------------------------------------------------------------
/// Parse the body of a LIST chunk whose type is INFO (starting after "INFO").
void parse_riff_info(ByteView body, TagMap& tags);
/// The complete LIST chunk ("LIST" size "INFO" ...), or empty when there is
/// nothing to write. Sub-chunks are padded to even sizes.
[[nodiscard]] Bytes riff_info_chunk(const TagMap& tags);

// ID3v2 ---------------------------------------------------------------------
/// Parse an ID3v2.2/2.3/2.4 tag (starting with "ID3"); text frames, COMM and
/// TXXX are read, the rest ignored. Never fails: damage ends parsing.
void parse_id3v2(ByteView tag, TagMap& tags);
/// An ID3v2.4 tag with UTF-8 text frames, or empty when there are no tags.
[[nodiscard]] Bytes id3v2_tag(const TagMap& tags);

// APEv2 (WavPack, TTA, Monkey's Audio) ------------------------------------------
/// Find an APEv2 tag at the end of the file (optionally before an ID3v1
/// tag), parse its text items into the map and return the tag's total size
/// (0 when there is none), so readers can stop before it.
std::size_t parse_apev2(ByteView file, TagMap& tags);
/// An APEv2 tag (header, items, footer), or empty when there are no tags.
[[nodiscard]] Bytes apev2_tag(const TagMap& tags);

// Vorbis comments -------------------------------------------------------------
/// Parse a Vorbis comment block (vendor string + list). Returns false when
/// it is malformed (whatever was read before the damage is kept).
bool parse_vorbis_comment(ByteView block, TagMap& tags);
/// Serialise (vendor string empty, NAME=value entries in upper case).
[[nodiscard]] Bytes vorbis_comment(const TagMap& tags);
/// Field name for a tag key ("track" -> "TRACKNUMBER") and back.
[[nodiscard]] std::string vorbis_field(std::string_view key);
[[nodiscard]] std::string key_from_vorbis(std::string_view field);

}  // namespace cvt::audio::tags
