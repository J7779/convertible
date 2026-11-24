// Convertible core - progress reporting and cooperative cancellation.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace cvt {

/// Handle through which a codec reports how far it is and learns whether the
/// user gave up. Cheap to copy; copies and sub-ranges share one cancellation
/// flag and one sink. A default-constructed Progress discards reports and is
/// never cancelled, so codecs can call it unconditionally. Thread-safe.
///
/// Long loops should report regularly and stop when asked:
///
///   for (std::uint32_t y = 0; y < height; ++y) {
///     if ((y & 63) == 0 && !ctx.progress.report(double(y) / height)) {
///       return fail(Status::Cancelled, "cancelled");
///     }
///     ...
///   }
///
/// or, shorter, `CVT_TRY(ctx.checkpoint(double(y) / height));`.
class Progress {
 public:
  /// Receives overall progress in 0..1 and a short stage label (may be
  /// empty). Return true to request cancellation.
  using Sink = std::function<bool(double fraction, std::string_view stage)>;

  Progress() = default;
  explicit Progress(Sink sink) : state_(std::make_shared<State>(std::move(sink))) {}

  /// Report progress within this handle's range. `fraction` is clamped to
  /// 0..1. Returns false once cancellation has been requested, true otherwise.
  bool report(double fraction, std::string_view stage = {}) const {
    if (!state_) return true;
    const double overall = begin_ + (end_ - begin_) * std::clamp(fraction, 0.0, 1.0);
    return state_->report(overall, stage.empty() ? std::string_view(stage_) : stage);
  }

  /// True once the sink has asked to stop (or `cancel()` was called).
  [[nodiscard]] bool cancelled() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_relaxed);
  }

  /// Request cancellation from the engine side.
  void cancel() const noexcept {
    if (state_) state_->cancelled.store(true, std::memory_order_relaxed);
  }

  /// A handle whose 0..1 maps onto `begin..end` of this one, for delegating
  /// part of the work: `decode_frames(ctx.progress.sub(0.1, 0.9))`.
  /// `stage` becomes the default label for reports made through it.
  [[nodiscard]] Progress sub(double begin, double end, std::string_view stage = {}) const {
    Progress child = *this;
    const double lo = std::clamp(begin, 0.0, 1.0);
    const double hi = std::clamp(end, lo, 1.0);
    child.begin_ = begin_ + (end_ - begin_) * lo;
    child.end_ = begin_ + (end_ - begin_) * hi;
    if (!stage.empty()) child.stage_ = std::string(stage);
    return child;
  }

 private:
  struct State {
    explicit State(Sink s) : sink(std::move(s)) {}
    bool report(double overall, std::string_view stage) {
      if (cancelled.load(std::memory_order_relaxed)) return false;
      if (!sink) return true;
      const std::scoped_lock lock(mutex);
      // Progress never runs backwards, and tiny steps are not worth a callback.
      overall = std::max(overall, last_fraction);
      const bool stage_changed = !stage.empty() && stage != last_stage;
      if (!stage_changed && overall < 1.0 && overall - last_fraction < 0.002 && reported_once) return true;
      if (overall >= 1.0 && last_fraction >= 1.0 && !stage_changed) return true;
      last_fraction = overall;
      if (stage_changed) last_stage = std::string(stage);
      reported_once = true;
      if (sink(overall, last_stage)) cancelled.store(true, std::memory_order_relaxed);
      return !cancelled.load(std::memory_order_relaxed);
    }
    Sink sink;
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    double last_fraction = 0.0;
    std::string last_stage;
    bool reported_once = false;
  };

  std::shared_ptr<State> state_;
  double begin_ = 0.0;
  double end_ = 1.0;
  std::string stage_;
};

}  // namespace cvt
