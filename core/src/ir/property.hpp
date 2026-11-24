// Convertible core - generic content-line property shared by the Calendar
// and Contacts IRs (the iCalendar / vCard data model).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cvt::ir {

/// A property the IR does not model explicitly, kept so that round trips do
/// not silently drop data: `NAME;PARAM=value:VALUE`.
struct Property {
  /// Upper-case property name, e.g. "X-APPLE-STRUCTURED-LOCATION".
  std::string name;
  /// Parameters in source order (name upper-case, value unescaped).
  std::vector<std::pair<std::string, std::string>> params;
  /// Unescaped, unfolded value (UTF-8).
  std::string value;
};

}  // namespace cvt::ir
