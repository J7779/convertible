// Convertible core - AnyIR: a type-safe holder for one IR value.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

#include "ir/archive.hpp"
#include "ir/audio.hpp"
#include "ir/blob.hpp"
#include "ir/calendar.hpp"
#include "ir/contacts.hpp"
#include "ir/data.hpp"
#include "ir/document.hpp"
#include "ir/font.hpp"
#include "ir/geo.hpp"
#include "ir/image.hpp"
#include "ir/kind.hpp"
#include "ir/mesh.hpp"
#include "ir/plain_text.hpp"
#include "ir/subtitles.hpp"
#include "ir/table.hpp"
#include "ir/video.hpp"

namespace cvt {

namespace detail {
// Alternative order MUST match `enum class Kind`.
using IrVariant = std::variant<ir::Blob, ir::Image, ir::Audio, ir::Document, ir::Data, ir::Table, ir::Archive,
                               ir::Subtitles, ir::Mesh, ir::Font, ir::Geo, ir::Calendar, ir::Contacts, ir::PlainText,
                               ir::Video>;

template <class T, class Variant>
struct index_in;
template <class T, class... Ts>
struct index_in<T, std::variant<Ts...>> {
  static constexpr std::size_t value = [] {
    constexpr std::array<bool, sizeof...(Ts)> matches = {std::same_as<T, Ts>...};
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (matches[i]) return i;
    }
    return matches.size();
  }();
};
}  // namespace detail

/// True for the IR value types (ir::Blob, ir::Image, ...).
template <class T>
concept IrType = detail::index_in<T, detail::IrVariant>::value < std::variant_size_v<detail::IrVariant>;

/// The `Kind` corresponding to an IR type: `kind_of<ir::Image> == Kind::Image`.
template <IrType T>
inline constexpr Kind kind_of = static_cast<Kind>(detail::index_in<T, detail::IrVariant>::value);

static_assert(std::variant_size_v<detail::IrVariant> == kind_count);
static_assert(kind_of<ir::Blob> == Kind::Blob && kind_of<ir::Image> == Kind::Image && kind_of<ir::Audio> == Kind::Audio &&
              kind_of<ir::Document> == Kind::Document && kind_of<ir::Data> == Kind::Data &&
              kind_of<ir::Table> == Kind::Table && kind_of<ir::Archive> == Kind::Archive &&
              kind_of<ir::Subtitles> == Kind::Subtitles && kind_of<ir::Mesh> == Kind::Mesh &&
              kind_of<ir::Font> == Kind::Font && kind_of<ir::Geo> == Kind::Geo &&
              kind_of<ir::Calendar> == Kind::Calendar && kind_of<ir::Contacts> == Kind::Contacts &&
              kind_of<ir::PlainText> == Kind::PlainText && kind_of<ir::Video> == Kind::Video);

/// Holds exactly one IR value of any kind. Codecs and bridges exchange values
/// through it. AnyIR is MOVE-ONLY, because some IRs (ir::Video) are streams
/// that cannot be duplicated: pass it by value and `std::move` it along.
///
///   Result<AnyIR> read_png(ReadContext& ctx) { ir::Image image; ...; return AnyIR(std::move(image)); }
///   Result<Bytes> write_png(const AnyIR& value, WriteContext& ctx) {
///     const ir::Image& image = value.get<ir::Image>();   // the engine guarantees the kind
///     ...
///   }
class AnyIR {
 public:
  /// An empty Blob.
  AnyIR() = default;

  /// Implicit from any IR value (copies an lvalue of a copyable IR type;
  /// move-only IRs must be passed as rvalues).
  template <class T>
    requires IrType<std::remove_cvref_t<T>>
  AnyIR(T&& value) : value_(std::forward<T>(value)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] Kind kind() const noexcept { return static_cast<Kind>(value_.index()); }

  template <IrType T>
  [[nodiscard]] bool is() const noexcept {
    return std::holds_alternative<T>(value_);
  }

  /// The held value. Precondition: `is<T>()`. The engine only ever calls a
  /// codec or bridge with the kind it registered for, so they may use this
  /// directly; violating the precondition throws std::bad_variant_access,
  /// which the engine reports as an internal error.
  /// Works on lvalues (returns a reference) and rvalues (returns T&&):
  ///   ir::Image image = std::move(any).get<ir::Image>();
  template <IrType T, class Self>
  [[nodiscard]] decltype(auto) get(this Self&& self) {
    return std::get<T>(std::forward<Self>(self).value_);
  }

  /// Pointer to the held value, or nullptr when it is of another kind.
  template <IrType T>
  [[nodiscard]] const T* get_if() const noexcept {
    return std::get_if<T>(&value_);
  }
  template <IrType T>
  [[nodiscard]] T* get_if() noexcept {
    return std::get_if<T>(&value_);
  }

  /// Visit the held value: `any.visit([](const auto& ir) { ... });`
  template <class Self, class Visitor>
  decltype(auto) visit(this Self&& self, Visitor&& visitor) {
    return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self).value_);
  }

 private:
  detail::IrVariant value_;
};

}  // namespace cvt
