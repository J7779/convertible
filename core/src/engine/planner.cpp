// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/planner.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <limits>
#include <map>
#include <tuple>

namespace cvt {

namespace {

constexpr std::size_t implicit_reader_order = std::numeric_limits<std::size_t>::max();

// The best known way to obtain a value of some kind from the source format.
struct Reach {
  bool valid = false;
  int cost = 0;
  int bridge_count = 0;
  int lossy_steps = 0;
  const Codec* reader = nullptr;  // nullptr = implicit Blob reader
  Kind reader_kind = Kind::Blob;
  std::array<const Bridge*, max_bridges_per_plan> bridges{};
  // Registration positions (reader, then bridges): the final, total tie-break.
  std::array<std::size_t, 1 + max_bridges_per_plan> order{};

  [[nodiscard]] auto key() const { return std::tie(cost, bridge_count, lossy_steps, order); }
};

bool better(const Reach& candidate, const Reach& incumbent) {
  if (!candidate.valid) return false;
  if (!incumbent.valid) return true;
  return candidate.key() < incumbent.key();
}

using ReachTable = std::array<Reach, kind_count>;

std::size_t index_of(Kind kind) { return static_cast<std::size_t>(kind); }

bool excludes(const Bridge& bridge, Category target) {
  return std::ranges::find(bridge.excluded_target_categories, target) != bridge.excluded_target_categories.end();
}

// Everything reachable from the source through a reader and up to two bridges,
// on the way to a target of category `target`.
ReachTable compute_reach(const Registry& registry, std::string_view source_id, bool allow_implicit_blob, Category target) {
  std::array<ReachTable, max_bridges_per_plan + 1> layers{};

  const std::span<const Codec> all_codecs = registry.codecs();
  bool explicit_blob_reader = false;
  for (const Codec* codec : registry.codecs_for(source_id)) {
    if (!codec->read) continue;
    if (codec->kind == Kind::Blob) explicit_blob_reader = true;
    Reach reach;
    reach.valid = true;
    reach.cost = codec->read_cost;
    reach.lossy_steps = codec->lossy ? 1 : 0;
    reach.reader = codec;
    reach.reader_kind = codec->kind;
    reach.order[0] = static_cast<std::size_t>(codec - all_codecs.data());
    Reach& slot = layers[0][index_of(codec->kind)];
    if (better(reach, slot)) slot = reach;
  }
  if (allow_implicit_blob && !explicit_blob_reader) {
    Reach reach;
    reach.valid = true;
    reach.cost = implicit_blob_read_cost;
    reach.reader_kind = Kind::Blob;
    reach.order[0] = implicit_reader_order;
    layers[0][index_of(Kind::Blob)] = reach;
  }

  const std::span<const Bridge> bridges = registry.bridges();
  for (int layer = 1; layer <= max_bridges_per_plan; ++layer) {
    const auto depth = static_cast<std::size_t>(layer);
    for (std::size_t b = 0; b < bridges.size(); ++b) {
      const Bridge& bridge = bridges[b];
      if (!bridge.fn || bridge.from == bridge.to || excludes(bridge, target)) continue;
      if (bridge.after_reader_only && layer > 1) continue;
      const Reach& from = layers[depth - 1][index_of(bridge.from)];
      if (!from.valid) continue;
      Reach reach = from;
      reach.cost += bridge.cost;
      reach.bridge_count = layer;
      reach.lossy_steps += bridge.lossy ? 1 : 0;
      reach.bridges[depth - 1] = &bridge;
      reach.order[depth] = b;
      Reach& slot = layers[depth][index_of(bridge.to)];
      if (better(reach, slot)) slot = reach;
    }
  }

  ReachTable best{};
  for (const ReachTable& layer : layers) {
    for (std::size_t k = 0; k < kind_count; ++k) {
      if (better(layer[k], best[k])) best[k] = layer[k];
    }
  }
  return best;
}

struct NativeChoice {
  bool valid = false;
  Reach reach;
  const Codec* writer = nullptr;
  int cost = 0;
  int lossy_steps = 0;
  std::size_t writer_order = 0;

  [[nodiscard]] auto key() const {
    return std::tie(cost, reach.bridge_count, lossy_steps, reach.order, writer_order);
  }
};

NativeChoice pick_writer(const Registry& registry, const ReachTable& reach, std::string_view source_id,
                         std::string_view target_id) {
  NativeChoice best;
  const std::span<const Codec> all_codecs = registry.codecs();
  for (const Codec* codec : registry.codecs_for(target_id)) {
    if (!codec->write) continue;
    if (codec->same_format_only && source_id != target_id) continue;
    if (!codec->only_from.empty() && std::ranges::find(codec->only_from, source_id) == codec->only_from.end()) continue;
    const Reach& from = reach[index_of(codec->kind)];
    if (!from.valid) continue;
    if (!codec->ir_tag.empty() &&
        (from.reader == nullptr || from.bridge_count != 0 || from.reader->ir_tag != codec->ir_tag)) {
      continue;
    }
    if (codec->direct_only && (from.reader == nullptr || from.bridge_count != 0)) continue;
    // A direct-only writer expects plain content of its kind, not another
    // reader's dialect (a vector scene is no .npmrc object).
    if (codec->direct_only && !from.reader->ir_tag.empty() && from.reader->ir_tag != codec->ir_tag) continue;
    NativeChoice choice;
    choice.valid = true;
    choice.reach = from;
    choice.writer = codec;
    choice.cost = from.cost + codec->write_cost;
    choice.lossy_steps = from.lossy_steps + (codec->lossy ? 1 : 0);
    choice.writer_order = static_cast<std::size_t>(codec - all_codecs.data());
    if (!best.valid || choice.key() < best.key()) best = choice;
  }
  return best;
}

Plan to_plan(const NativeChoice& choice, const FormatInfo* source, const FormatInfo* target) {
  Plan plan;
  plan.source = source;
  plan.target = target;
  plan.reader = choice.reach.reader;
  plan.reader_kind = choice.reach.reader_kind;
  for (int i = 0; i < choice.reach.bridge_count; ++i) {
    plan.bridges.push_back(choice.reach.bridges[static_cast<std::size_t>(i)]);
  }
  plan.writer = choice.writer;
  plan.cost = choice.cost;
  plan.lossy = choice.lossy_steps > 0;
  return plan;
}

// Targets ranked with the source's own category: the same category, and the
// few neighbours users reach for first - a drawing becomes a picture or a
// PDF, data becomes a spreadsheet and back, any page-like document a PDF.
// 0: ranked with the source's own category (a drawing's pictures and PDF);
// 1: right after it (data <-> spreadsheets, PDF for spreadsheets, e-books and
// presentations); 2: everything else.
int closeness(const FormatInfo& source, const FormatInfo& target) {
  if (target.category == source.category) return 0;
  switch (source.category) {
    case Category::Vector: return target.category == Category::Image || target.id == "pdf" ? 0 : 2;
    case Category::Data: return target.category == Category::Spreadsheet ? 1 : 2;
    case Category::Spreadsheet: return target.category == Category::Data || target.id == "pdf" ? 1 : 2;
    case Category::Ebook:
    case Category::Presentation: return target.id == "pdf" || target.id == "docx" ? 1 : 2;
    default: return 2;
  }
}

void add_specs(std::vector<OptionSpec>& out, const std::vector<OptionSpec>& specs) {
  for (const OptionSpec& spec : specs) {
    const bool known = std::ranges::any_of(out, [&](const OptionSpec& existing) { return existing.key == spec.key; });
    if (!known) out.push_back(spec);
  }
}

}  // namespace

std::vector<OptionSpec> Plan::option_specs() const {
  std::vector<OptionSpec> specs;
  if (reader != nullptr) add_specs(specs, reader->read_options);
  for (const Bridge* bridge : bridges) add_specs(specs, bridge->options);
  if (writer != nullptr) add_specs(specs, writer->write_options);
  return specs;
}

std::string Plan::describe() const {
  std::string out = source != nullptr ? source->id : std::string("?");
  out += " -> ";
  out += kind_name(reader_kind);
  if (reader == nullptr) out += " (raw bytes)";
  for (const Bridge* bridge : bridges) {
    out += " -> [" + (bridge->label.empty() ? std::string("bridge") : bridge->label) + "] -> ";
    out += kind_name(bridge->to);
  }
  out += " -> ";
  out += target != nullptr ? target->id : std::string("?");
  return out;
}

Result<Plan> plan_conversion(const Registry& registry, std::string_view source_id, std::string_view target_id) {
  const FormatInfo* source = registry.find(source_id);
  if (source == nullptr) return fail(Status::UnknownFormat, "unknown source format '{}'", source_id);
  const FormatInfo* target = registry.find(target_id);
  if (target == nullptr) return fail(Status::UnknownFormat, "unknown target format '{}'", target_id);
  if (source->output_only) {
    return fail(Status::UnsupportedConversion, "'{}' is an output-only format: it can be produced but not converted from",
                source->id);
  }

  const bool identity = source == target;
  const ReachTable reach = compute_reach(registry, source->id, !identity, target->category);
  if (const NativeChoice native = pick_writer(registry, reach, source->id, target->id); native.valid) {
    return to_plan(native, source, target);
  }
  return fail(Status::UnsupportedConversion, "no conversion from '{}' to '{}' is available", source->id, target->id);
}

std::vector<TargetInfo> list_targets(const Registry& registry, std::string_view source_id) {
  std::vector<TargetInfo> out;
  const FormatInfo* source = registry.find(source_id);
  if (source == nullptr || source->output_only) return out;

  // Reachability depends on the target category (bridge exclusions): one
  // table per category, computed on first use.
  std::map<Category, ReachTable> reach_by_category;
  const auto reach_for = [&](Category category) -> const ReachTable& {
    auto it = reach_by_category.find(category);
    if (it == reach_by_category.end()) it = reach_by_category.emplace(category, compute_reach(registry, source->id, true, category)).first;
    return it->second;
  };
  const ReachTable identity_reach = compute_reach(registry, source->id, false, source->category);
  for (const FormatInfo* format : registry.formats()) {
    const bool identity = format == source;
    const NativeChoice native =
        pick_writer(registry, identity ? identity_reach : reach_for(format->category), source->id, format->id);
    if (native.valid) {
      out.push_back({format, native.lossy_steps > 0, native.cost, identity, native.reach.bridge_count,
                     native.reach.reader == nullptr});
    }
  }

  // What a user expects first: formats of the source's own category, most
  // popular first (a text file offers PDF before rarer text formats, a PNG
  // offers JPEG, a WAV offers FLAC); a lossy target is not demoted there,
  // since a lossy re-encode is usually the point. Then real conversions to
  // other categories, closest first (fewer bridges: the sound of a video
  // before its pictures), faithful before lossy, by popularity. The universal
  // byte-level targets (Base64, checksums ...) come last. The same-format
  // "compress" entry is flagged for UIs and sorts like any other.
  std::ranges::stable_sort(out, [source](const TargetInfo& a, const TargetInfo& b) {
    const int a_rank = a.byte_level ? 3 : closeness(*source, *a.format);
    const int b_rank = b.byte_level ? 3 : closeness(*source, *b.format);
    const bool a_same = a_rank < 2;
    const bool b_same = b_rank < 2;
    if (a_same != b_same) return a_same;
    if (a_same) {
      // Neighbours (rank 1) count a little less popular than the source's own kind.
      const int a_score = a.format->popularity - 10 * a_rank;
      const int b_score = b.format->popularity - 10 * b_rank;
      if (a_score != b_score) return a_score > b_score;
    }
    if (!a_same) {
      if (a.byte_level != b.byte_level) return !a.byte_level;
      if (a.bridges != b.bridges) return a.bridges < b.bridges;
      if (a.lossy != b.lossy) return !a.lossy;
    }
    if (a.format->popularity != b.format->popularity) return a.format->popularity > b.format->popularity;
    return a.format->id < b.format->id;
  });
  return out;
}

std::string_view target_role(const FormatInfo& source, const TargetInfo& target) {
  if (target.same_format) return "compress";
  if (target.format->id == "extract") return "extract";
  if (target.format->category == Category::Archive && source.category != Category::Archive) return "archive";
  if (target.byte_level) return "encode";
  return "convert";
}

}  // namespace cvt
