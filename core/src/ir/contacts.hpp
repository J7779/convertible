// Convertible core - Contacts IR: address-book cards (the vCard model).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <vector>

#include "ir/property.hpp"
#include "util/bytes.hpp"

namespace cvt::ir {

/// A value with a role label, used for e-mail addresses, phones and URLs.
struct LabeledValue {
  std::string value;
  /// Lower-case type such as "home", "work", "cell", "fax"; may be empty.
  std::string type;
};

struct PostalAddress {
  std::string street;       ///< may contain '\n' for multiple lines
  std::string city;         ///< locality
  std::string region;       ///< state / province
  std::string postal_code;
  std::string country;
  std::string type;         ///< "home", "work", ... ; may be empty
};

/// One person or organisation (VCARD).
struct Card {
  /// Formatted display name (FN). Writers derive it from the parts below
  /// when empty.
  std::string full_name;
  std::string family_name;      ///< N component 1
  std::string given_name;       ///< N component 2
  std::string additional_name;  ///< N component 3 (middle names)
  std::string prefix;           ///< N component 4 ("Dr.")
  std::string suffix;           ///< N component 5 ("Jr.")
  std::string nickname;
  std::string organization;
  std::string title;            ///< job title
  std::vector<LabeledValue> emails;
  std::vector<LabeledValue> phones;
  std::vector<PostalAddress> addresses;
  std::vector<LabeledValue> urls;
  /// ISO 8601 date ("1990-04-27", or "--04-27" without a year); empty when unknown.
  std::string birthday;
  std::string note;             ///< free text; lines separated by '\n'
  Bytes photo;                  ///< embedded picture file; empty when none
  std::string photo_mime;       ///< e.g. "image/jpeg"
  std::string uid;
  /// Properties not modelled above, preserved for round trips.
  std::vector<Property> extra;
};

/// An address book.
struct Contacts {
  std::vector<Card> cards;
};

}  // namespace cvt::ir
