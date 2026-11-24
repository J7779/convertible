// Convertible core - Subtitles IR: timed text cues.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cvt::ir {

/// Where a cue is drawn, for formats that support positioning.
struct CuePosition {
  /// Anchor in numeric-keypad layout, as in ASS `\an`: 1-3 bottom
  /// (left, centre, right), 4-6 middle, 7-9 top. Default 2 = bottom centre.
  int alignment = 2;
  /// Optional explicit anchor point as a fraction of the video frame,
  /// 0.0 = left/top edge, 1.0 = right/bottom edge.
  std::optional<double> x;
  std::optional<double> y;
};

/// One subtitle.
struct Cue {
  /// Display interval in milliseconds from the start of the media.
  /// `end_ms >= start_ms`.
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  /// UTF-8 text; lines separated by '\n'. May contain only these inline
  /// tags: <i>, <b>, <u> and their closing forms. Readers convert
  /// format-specific markup to these (or strip it); writers that cannot
  /// express a tag must strip it.
  std::string text;
  /// Speaker / actor name when the format records one.
  std::optional<std::string> speaker;
  /// Name of a style defined by the source format (ASS style, TTML region).
  std::optional<std::string> style;
  /// Explicit placement; nullopt = the player's default (bottom centre).
  std::optional<CuePosition> position;
  /// Format-specific fields kept for round trips through the same format,
  /// keyed "<format>.<field>" (e.g. "ass.layer", "ass.effect", "vtt.id",
  /// "vtt.settings"). Writers of other formats ignore them.
  std::map<std::string, std::string> attributes;
  /// The cue text in the source format's own markup when that carries more
  /// than `text` can express (ASS override tags such as colours or karaoke),
  /// together with the id of that format. A writer of the same format may
  /// use it instead of `text` as long as it still renders to the same plain
  /// text; empty when not needed.
  std::string source_text;
  std::string source_format;
};

/// A subtitle track.
struct Subtitles {
  /// Cues, sorted by `start_ms` (readers sort; overlapping cues are allowed).
  std::vector<Cue> cues;
  /// Track-level metadata ("title", "language" as a BCP 47 tag, ...).
  std::map<std::string, std::string> metadata;
};

}  // namespace cvt::ir
