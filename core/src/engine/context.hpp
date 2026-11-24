// Convertible core - the context handed to codecs and bridges.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/errors.hpp"
#include "engine/limits.hpp"
#include "engine/options.hpp"
#include "engine/progress.hpp"
#include "util/bytes.hpp"

namespace cvt {

class Registry;

/// Collects non-fatal notes about a conversion ("3 characters could not be
/// represented", "animation flattened to its first frame"). They are shown
/// to the user next to the result. Copies share one list. Thread-safe.
/// Duplicates are dropped and the list is capped so a noisy loop cannot
/// flood it.
class Warnings {
 public:
  static constexpr std::size_t max_messages = 50;

  Warnings() : state_(std::make_shared<State>()) {}

  void add(std::string message) const {
    const std::scoped_lock lock(state_->mutex);
    for (const std::string& existing : state_->messages) {
      if (existing == message) return;
    }
    if (state_->messages.size() < max_messages) state_->messages.push_back(std::move(message));
  }
  [[nodiscard]] std::vector<std::string> list() const {
    const std::scoped_lock lock(state_->mutex);
    return state_->messages;
  }
  [[nodiscard]] bool empty() const {
    const std::scoped_lock lock(state_->mutex);
    return state_->messages.empty();
  }

 private:
  struct State {
    std::mutex mutex;
    std::vector<std::string> messages;
  };
  std::shared_ptr<State> state_;
};

/// State shared by every step of one conversion. Bridges receive exactly
/// this; codecs receive the derived ReadContext / WriteContext.
struct Context {
  /// Option values, already validated against the step's OptionSpecs.
  Options options;
  /// Progress sink and cancellation flag, scoped to this step's share of the
  /// whole conversion (report 0..1 for your own work).
  Progress progress;
  /// Safety ceilings. Check them before allocating from input-derived sizes.
  Limits limits;
  /// Sink for non-fatal notes.
  Warnings warnings;
  /// The registry the conversion runs against (never null inside the
  /// engine; may be null in hand-built contexts in unit tests).
  const Registry* registry = nullptr;

  /// Add a user-visible note; the conversion continues.
  void warn(std::string message) const { warnings.add(std::move(message)); }

  /// Report progress and turn a cancellation request into an error:
  ///   CVT_TRY(ctx.checkpoint(done / total, "Decoding"));
  [[nodiscard]] Result<void> checkpoint(double fraction, std::string_view stage = {}) const {
    if (!progress.report(fraction, stage)) return fail(Status::Cancelled, "the conversion was cancelled");
    return {};
  }
};

/// Passed to `Codec::read`.
struct ReadContext : Context {
  /// The complete source file (possibly a memory-mapped view of a file on
  /// disk, so it can be far larger than what a codec should copy). The engine
  /// keeps these bytes alive and unchanged until the writer of the conversion
  /// has returned, so the IR - in particular streaming IRs such as ir::Video -
  /// may hold views into it. Nothing else in this context outlives `read`.
  ByteView input;
  /// Source file name without directories (UTF-8); may be empty.
  std::string filename;
  /// Canonical id of the format being read (useful when one function serves
  /// several formats).
  std::string format_id;
};

/// Passed to `Codec::write`.
struct WriteContext : Context {
  /// Canonical id of the format to produce.
  std::string format_id;
  /// Name of the original input file without directories; may be empty.
  std::string source_filename;
  /// Canonical id of the original input format; may be empty.
  std::string source_format_id;
  /// Set by a writer whose output is a file it can name ("extract": the one
  /// entry of an archive, "notes.txt" out of "notes.txt.gz"). The engine then
  /// reports that name and the format detected from it and the bytes, instead
  /// of the target format's extension and MIME type.
  std::string output_filename;
};

}  // namespace cvt
