// Convertible core - Data IR: structured, JSON-shaped data.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string_view>

#include "util/json.hpp"

namespace cvt::ir {

/// A tree of objects, arrays, strings, numbers, booleans and nulls: the
/// common model of JSON, YAML, TOML, XML-as-data, INI, plist, MessagePack...
struct Data {
  /// The document root. Objects keep their key order. Strings are UTF-8.
  /// Values that have no JSON equivalent are mapped by the reader (dates to
  /// ISO 8601 strings, binary to Base64 strings) and noted in a warning.
  /// The data family's readers instead use the tagged objects described in
  /// `data_tags` by default (selectable per conversion), so that such values
  /// survive a trip between formats that can hold them.
  Json value;
};

/// Tagged objects: how values without a JSON equivalent are carried inside a
/// `Data` tree. A tagged value is an object whose members are exactly the
/// ones listed (in any order); anything else is ordinary data.
///
///   {"$binary": "<base64>"}                      byte string
///   {"$binary": "<base64>", "$subtype": n}       byte string with a BSON subtype
///   {"$ext": n, "$binary": "<base64>"}           MessagePack extension type n
///   {"$date": "<ISO 8601 text>"}                 date and/or time; the text says which:
///                                                "2024-05-01T12:30:00.5Z" / "+02:00" (instant),
///                                                "2024-05-01T12:30:00" (local date-time),
///                                                "2024-05-01" (date), "12:30:00" (time of day)
///   {"$bigint": "<decimal digits>"}              integer beyond the 64-bit range
///   {"$decimal": "<decimal text>"}               number a double cannot hold exactly
///   {"$float": "NaN" | "Infinity" | "-Infinity"} non-finite number (read back as a double)
///   {"$uid": n}                                  Apple property-list UID
///   {"$tag": n, "$value": v}                     CBOR tag n without a dedicated mapping
///   {"$undefined": true}                         CBOR / BSON "undefined"
///   {"$oid": "<24 hex digits>"}                  BSON ObjectId
///   {"$regex": "<pattern>", "$options": "<flags>"}   BSON regular expression
///   {"$timestamp": {"t": seconds, "i": increment}}   BSON internal timestamp
///   {"$minKey": 1}, {"$maxKey": 1}               BSON min / max key
///   {"$code": "<source>"}                        BSON JavaScript code
///   {"$symbol": "<name>"}                        symbol (BSON, Ion, EDN, S-expressions)
namespace data_tags {
inline constexpr std::string_view binary = "$binary";
inline constexpr std::string_view subtype = "$subtype";
inline constexpr std::string_view ext = "$ext";
inline constexpr std::string_view date = "$date";
inline constexpr std::string_view bigint = "$bigint";
inline constexpr std::string_view decimal = "$decimal";
inline constexpr std::string_view float_value = "$float";
inline constexpr std::string_view uid = "$uid";
inline constexpr std::string_view tag = "$tag";
inline constexpr std::string_view tag_value = "$value";
inline constexpr std::string_view undefined = "$undefined";
inline constexpr std::string_view object_id = "$oid";
inline constexpr std::string_view regex = "$regex";
inline constexpr std::string_view regex_options = "$options";
inline constexpr std::string_view timestamp = "$timestamp";
inline constexpr std::string_view min_key = "$minKey";
inline constexpr std::string_view max_key = "$maxKey";
inline constexpr std::string_view code = "$code";
inline constexpr std::string_view symbol = "$symbol";
}  // namespace data_tags

}  // namespace cvt::ir
