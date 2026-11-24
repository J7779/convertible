// Convertible core - route planning: which reader, bridges and writer turn a
// source format into a target format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/errors.hpp"
#include "engine/registry.hpp"

namespace cvt {

/// The planner never chains more than this many bridges.
inline constexpr int max_bridges_per_plan = 2;

/// A concrete way to perform one conversion:
/// `reader -> bridges... -> writer`, all through the IR.
/// Pointers refer into the registry the plan was made from.
struct Plan {
  const FormatInfo* source = nullptr;
  const FormatInfo* target = nullptr;
  /// Codec whose `read` is used; nullptr means the engine's implicit
  /// "whole file as ir::Blob" reader.
  const Codec* reader = nullptr;
  /// Kind produced by the reader step.
  Kind reader_kind = Kind::Blob;
  /// Zero to `max_bridges_per_plan` bridges, in execution order.
  std::vector<const Bridge*> bridges;
  /// Codec whose `write` is used. Never null in a plan returned by the planner.
  const Codec* writer = nullptr;
  /// Sum of the step costs.
  int cost = 0;
  /// True when any step discards information.
  bool lossy = false;

  /// Options of every step, in execution order, first declaration of a key wins.
  [[nodiscard]] std::vector<OptionSpec> option_specs() const;
  /// Human-readable route for logs and tests, e.g.
  /// "csv -> Table -> [render table as document] -> Document -> pdf".
  [[nodiscard]] std::string describe() const;
};

/// Choose the cheapest route from `source_id` to `target_id` (canonical ids),
/// skipping bridges that exclude the target's category and same-format-only
/// writers when the formats differ:
/// reader(source, k1) -> 0..2 bridges -> writer(target, k2), minimising the
/// summed cost. Ties are broken deterministically: fewer bridges, then fewer
/// lossy steps, then earlier registration.
///
/// A conversion to the same format ("compress": png -> png, mp4 -> mp4,
/// zip -> zip) is a first-class route, planned through an explicit reader so
/// that it is a real re-encode, never a byte copy via the implicit Blob reader.
///
/// Errors: Status::UnknownFormat for unregistered ids,
/// Status::UnsupportedConversion when there is no route.
[[nodiscard]] Result<Plan> plan_conversion(const Registry& registry, std::string_view source_id,
                                           std::string_view target_id);

/// One entry of the "convert to..." list.
struct TargetInfo {
  const FormatInfo* format = nullptr;
  bool lossy = false;
  int cost = 0;
  /// True when the target is the source format itself: a re-encode, offered
  /// by UIs as "Compress" / "Optimise".
  bool same_format = false;
  /// Bridges on the route (0 = the target reads the same kind as the source).
  int bridges = 0;
  /// True when the route only sees the file's bytes (the implicit Blob
  /// reader): Base64, hex, checksums and other universal byte targets.
  bool byte_level = false;
};

/// Every format `source_id` can be converted to, most relevant first: the
/// source's own category by popularity; then real conversions to other
/// categories (fewer bridges first, then lossless before lossy, then by
/// popularity); then the universal byte-level targets; ties by id. Routes through a bridge that
/// excludes the target's category (Bridge::excluded_target_categories) and
/// same-format-only writers of other formats are never offered.
/// Empty when the source id is unknown.
[[nodiscard]] std::vector<TargetInfo> list_targets(const Registry& registry, std::string_view source_id);

}  // namespace cvt
