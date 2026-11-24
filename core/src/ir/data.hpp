// Convertible core - Data IR: structured, JSON-shaped data.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "util/json.hpp"

namespace cvt::ir {

/// A tree of objects, arrays, strings, numbers, booleans and nulls: the
/// common model of JSON, YAML, TOML, XML-as-data, INI, plist, MessagePack...
struct Data {
  /// The document root. Objects keep their key order. Strings are UTF-8.
  /// Values that have no JSON equivalent are mapped by the reader (dates to
  /// ISO 8601 strings, binary to Base64 strings) and noted in a warning.
  Json value;
};

}  // namespace cvt::ir
