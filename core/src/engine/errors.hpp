// Convertible core - error model.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cvt {

/// Outcome category of an engine operation. The numeric values are part of
/// the C ABI (`enum cvt_status` in convertible.h) and must never change.
enum class Status : int {
  Ok = 0,
  InvalidArgument = 1,       ///< bad argument or option value
  UnknownFormat = 2,         ///< format id not registered / detection failed
  UnsupportedConversion = 3, ///< no route from source to target
  Decode = 4,                ///< the input is malformed or uses an unsupported feature
  Encode = 5,                ///< the IR cannot be represented in the target format
  Io = 6,                    ///< file system failure
  Cancelled = 7,             ///< the progress callback asked to stop
  Limit = 8,                 ///< a safety limit was exceeded (size, pixels, entries, ...)
  Internal = 9,              ///< bug or unexpected exception
};

[[nodiscard]] constexpr std::string_view status_name(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid argument";
    case Status::UnknownFormat: return "unknown format";
    case Status::UnsupportedConversion: return "unsupported conversion";
    case Status::Decode: return "decode error";
    case Status::Encode: return "encode error";
    case Status::Io: return "I/O error";
    case Status::Cancelled: return "cancelled";
    case Status::Limit: return "limit exceeded";
    case Status::Internal: return "internal error";
  }
  return "internal error";
}

/// Failure description. `message` is UTF-8, human readable, and safe to show
/// to end users (no paths other than ones the caller supplied).
struct Error {
  Status code = Status::Internal;
  std::string message;
};

/// Every fallible engine, codec, bridge and utility function returns this.
/// Exceptions are not part of any interface in the core.
template <class T>
using Result = std::expected<T, Error>;

/// `return fail(Status::Decode, "truncated header");`
[[nodiscard]] inline std::unexpected<Error> fail(Status code, std::string_view message) {
  return std::unexpected(Error{code, std::string(message)});
}

/// `return fail(Status::Decode, "bad chunk size {} at offset {}", size, offset);`
template <class... Args>
  requires(sizeof...(Args) > 0)
[[nodiscard]] std::unexpected<Error> fail(Status code, std::format_string<Args...> fmt, Args&&... args) {
  return std::unexpected(Error{code, std::format(fmt, std::forward<Args>(args)...)});
}

/// Re-wrap an existing error for a different `Result<U>`.
[[nodiscard]] inline std::unexpected<Error> fail(Error error) {
  return std::unexpected(std::move(error));
}

}  // namespace cvt

#define CVT_DETAIL_CONCAT2(a, b) a##b
#define CVT_DETAIL_CONCAT(a, b) CVT_DETAIL_CONCAT2(a, b)

/// Propagate the error of a `Result<...>` expression, discarding its value:
///   CVT_TRY(ctx.checkpoint(0.5));
#define CVT_TRY(expr)                                                       \
  do {                                                                      \
    auto&& cvt_try_result_ = (expr);                                        \
    if (!cvt_try_result_) {                                                 \
      return ::std::unexpected(::std::move(cvt_try_result_).error());       \
    }                                                                       \
  } while (false)

#define CVT_DETAIL_TRY_ASSIGN(tmp, decl, expr)                              \
  auto tmp = (expr);                                                        \
  if (!tmp) {                                                               \
    return ::std::unexpected(::std::move(tmp).error());                     \
  }                                                                         \
  decl = ::std::move(*tmp)

/// Unwrap a `Result<T>` into a declaration or an existing variable,
/// propagating the error:
///   CVT_TRY_ASSIGN(auto header, parse_header(reader));
///   CVT_TRY_ASSIGN(image.frames, decode_frames(reader));
/// Expands to several statements: do not use as the unbraced body of an `if`.
#define CVT_TRY_ASSIGN(decl, expr) \
  CVT_DETAIL_TRY_ASSIGN(CVT_DETAIL_CONCAT(cvt_try_tmp_, __LINE__), decl, expr)
