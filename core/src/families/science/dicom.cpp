// Convertible core - science family: DICOM (PS3.10 files and raw data sets).
//
// Read: explicit and implicit VR, little and big endian, deflated transfer
// syntax, sequences and items of defined or undefined length, uncompressed
// and RLE Lossless pixel data natively, other encapsulated (JPEG, JPEG-LS,
// JPEG 2000) frames through the registry's image codecs when present.
// Images honour rescale, window/level, MONOCHROME1/2, RGB, YBR_FULL(_422) and
// palette colour, multi-frame -> frames. Tags become Data; patient- and
// institution-identifying elements, UIDs and private tags are removed unless
// strip_metadata is false.
// Write: a Secondary Capture image (grey or true colour, 8-bit, multi-frame)
// with deterministic UIDs and empty patient/study identification.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#include "engine/standard_options.hpp"
#include "families/science/imaging.hpp"
#include "families/science/science.hpp"
#include "util/deflate.hpp"
#include "util/hash.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

constexpr std::uint32_t tag(std::uint16_t g, std::uint16_t e) { return (std::uint32_t{g} << 16) | e; }

struct DictEntry {
  std::uint32_t tag;
  std::string_view vr;
  std::string_view keyword;
};

// The subset of the PS3.6 data dictionary needed to interpret implicit-VR
// files and to name common elements.
constexpr DictEntry dictionary[] = {
    {tag(0x0002, 0x0000), "UL", "FileMetaInformationGroupLength"}, {tag(0x0002, 0x0001), "OB", "FileMetaInformationVersion"},
    {tag(0x0002, 0x0002), "UI", "MediaStorageSOPClassUID"}, {tag(0x0002, 0x0003), "UI", "MediaStorageSOPInstanceUID"},
    {tag(0x0002, 0x0010), "UI", "TransferSyntaxUID"}, {tag(0x0002, 0x0012), "UI", "ImplementationClassUID"},
    {tag(0x0002, 0x0013), "SH", "ImplementationVersionName"}, {tag(0x0002, 0x0016), "AE", "SourceApplicationEntityTitle"},
    {tag(0x0008, 0x0005), "CS", "SpecificCharacterSet"}, {tag(0x0008, 0x0008), "CS", "ImageType"},
    {tag(0x0008, 0x0012), "DA", "InstanceCreationDate"}, {tag(0x0008, 0x0013), "TM", "InstanceCreationTime"},
    {tag(0x0008, 0x0016), "UI", "SOPClassUID"}, {tag(0x0008, 0x0018), "UI", "SOPInstanceUID"},
    {tag(0x0008, 0x0020), "DA", "StudyDate"}, {tag(0x0008, 0x0021), "DA", "SeriesDate"}, {tag(0x0008, 0x0022), "DA", "AcquisitionDate"},
    {tag(0x0008, 0x0023), "DA", "ContentDate"}, {tag(0x0008, 0x0030), "TM", "StudyTime"}, {tag(0x0008, 0x0031), "TM", "SeriesTime"},
    {tag(0x0008, 0x0032), "TM", "AcquisitionTime"}, {tag(0x0008, 0x0033), "TM", "ContentTime"},
    {tag(0x0008, 0x0050), "SH", "AccessionNumber"}, {tag(0x0008, 0x0060), "CS", "Modality"}, {tag(0x0008, 0x0064), "CS", "ConversionType"},
    {tag(0x0008, 0x0070), "LO", "Manufacturer"}, {tag(0x0008, 0x0080), "LO", "InstitutionName"},
    {tag(0x0008, 0x0081), "ST", "InstitutionAddress"}, {tag(0x0008, 0x0090), "PN", "ReferringPhysicianName"},
    {tag(0x0008, 0x0092), "ST", "ReferringPhysicianAddress"}, {tag(0x0008, 0x0094), "SH", "ReferringPhysicianTelephoneNumbers"},
    {tag(0x0008, 0x1010), "SH", "StationName"}, {tag(0x0008, 0x1030), "LO", "StudyDescription"},
    {tag(0x0008, 0x103E), "LO", "SeriesDescription"}, {tag(0x0008, 0x1040), "LO", "InstitutionalDepartmentName"},
    {tag(0x0008, 0x1048), "PN", "PhysiciansOfRecord"}, {tag(0x0008, 0x1050), "PN", "PerformingPhysicianName"},
    {tag(0x0008, 0x1060), "PN", "NameOfPhysiciansReadingStudy"}, {tag(0x0008, 0x1070), "PN", "OperatorsName"},
    {tag(0x0008, 0x1090), "LO", "ManufacturerModelName"}, {tag(0x0008, 0x1140), "SQ", "ReferencedImageSequence"},
    {tag(0x0008, 0x1150), "UI", "ReferencedSOPClassUID"}, {tag(0x0008, 0x1155), "UI", "ReferencedSOPInstanceUID"},
    {tag(0x0010, 0x0010), "PN", "PatientName"}, {tag(0x0010, 0x0020), "LO", "PatientID"},
    {tag(0x0010, 0x0030), "DA", "PatientBirthDate"}, {tag(0x0010, 0x0040), "CS", "PatientSex"}, {tag(0x0010, 0x1010), "AS", "PatientAge"},
    {tag(0x0010, 0x1020), "DS", "PatientSize"}, {tag(0x0010, 0x1030), "DS", "PatientWeight"}, {tag(0x0010, 0x1040), "LO", "PatientAddress"},
    {tag(0x0010, 0x2154), "SH", "PatientTelephoneNumbers"}, {tag(0x0010, 0x4000), "LT", "PatientComments"},
    {tag(0x0018, 0x0015), "CS", "BodyPartExamined"}, {tag(0x0018, 0x0050), "DS", "SliceThickness"},
    {tag(0x0018, 0x0060), "DS", "KVP"}, {tag(0x0018, 0x0088), "DS", "SpacingBetweenSlices"}, {tag(0x0018, 0x1000), "LO", "DeviceSerialNumber"},
    {tag(0x0018, 0x1020), "LO", "SoftwareVersions"}, {tag(0x0018, 0x5100), "CS", "PatientPosition"},
    {tag(0x0020, 0x000D), "UI", "StudyInstanceUID"}, {tag(0x0020, 0x000E), "UI", "SeriesInstanceUID"},
    {tag(0x0020, 0x0010), "SH", "StudyID"}, {tag(0x0020, 0x0011), "IS", "SeriesNumber"}, {tag(0x0020, 0x0012), "IS", "AcquisitionNumber"},
    {tag(0x0020, 0x0013), "IS", "InstanceNumber"}, {tag(0x0020, 0x0020), "CS", "PatientOrientation"},
    {tag(0x0020, 0x0032), "DS", "ImagePositionPatient"}, {tag(0x0020, 0x0037), "DS", "ImageOrientationPatient"},
    {tag(0x0020, 0x0052), "UI", "FrameOfReferenceUID"}, {tag(0x0020, 0x1041), "DS", "SliceLocation"},
    {tag(0x0028, 0x0002), "US", "SamplesPerPixel"}, {tag(0x0028, 0x0004), "CS", "PhotometricInterpretation"},
    {tag(0x0028, 0x0006), "US", "PlanarConfiguration"}, {tag(0x0028, 0x0008), "IS", "NumberOfFrames"},
    {tag(0x0028, 0x0010), "US", "Rows"}, {tag(0x0028, 0x0011), "US", "Columns"}, {tag(0x0028, 0x0030), "DS", "PixelSpacing"},
    {tag(0x0028, 0x0100), "US", "BitsAllocated"}, {tag(0x0028, 0x0101), "US", "BitsStored"}, {tag(0x0028, 0x0102), "US", "HighBit"},
    {tag(0x0028, 0x0103), "US", "PixelRepresentation"}, {tag(0x0028, 0x0106), "US", "SmallestImagePixelValue"},
    {tag(0x0028, 0x0107), "US", "LargestImagePixelValue"}, {tag(0x0028, 0x1050), "DS", "WindowCenter"},
    {tag(0x0028, 0x1051), "DS", "WindowWidth"}, {tag(0x0028, 0x1052), "DS", "RescaleIntercept"}, {tag(0x0028, 0x1053), "DS", "RescaleSlope"},
    {tag(0x0028, 0x1054), "LO", "RescaleType"}, {tag(0x0028, 0x1101), "US", "RedPaletteColorLookupTableDescriptor"},
    {tag(0x0028, 0x1102), "US", "GreenPaletteColorLookupTableDescriptor"}, {tag(0x0028, 0x1103), "US", "BluePaletteColorLookupTableDescriptor"},
    {tag(0x0028, 0x1201), "OW", "RedPaletteColorLookupTableData"}, {tag(0x0028, 0x1202), "OW", "GreenPaletteColorLookupTableData"},
    {tag(0x0028, 0x1203), "OW", "BluePaletteColorLookupTableData"}, {tag(0x0028, 0x2110), "CS", "LossyImageCompression"},
    {tag(0x0032, 0x1032), "PN", "RequestingPhysician"}, {tag(0x0032, 0x1060), "LO", "RequestedProcedureDescription"},
    {tag(0x0040, 0x0244), "DA", "PerformedProcedureStepStartDate"}, {tag(0x0040, 0x0253), "SH", "PerformedProcedureStepID"},
    {tag(0x7FE0, 0x0010), "OW", "PixelData"}, {tag(0xFFFE, 0xE000), "", "Item"}, {tag(0xFFFE, 0xE00D), "", "ItemDelimitationItem"},
    {tag(0xFFFE, 0xE0DD), "", "SequenceDelimitationItem"},
};

const DictEntry* lookup(std::uint32_t t) {
  for (const DictEntry& e : dictionary) {
    if (e.tag == t) return &e;
  }
  return nullptr;
}

/// Elements removed by default (PS3.15 basic profile, the subset this reader names) plus private groups.
bool identifying(std::uint32_t t) {
  const std::uint16_t g = static_cast<std::uint16_t>(t >> 16);
  if (g == 0x0010 || g == 0x0032 || g == 0x0038 || (g & 1) != 0) return true;
  static constexpr std::uint32_t list[] = {
      tag(0x0002, 0x0003), tag(0x0008, 0x0012), tag(0x0008, 0x0013), tag(0x0008, 0x0018), tag(0x0008, 0x0020),
      tag(0x0008, 0x0021), tag(0x0008, 0x0022), tag(0x0008, 0x0023), tag(0x0008, 0x0030), tag(0x0008, 0x0031),
      tag(0x0008, 0x0032), tag(0x0008, 0x0033), tag(0x0008, 0x0050), tag(0x0008, 0x0080), tag(0x0008, 0x0081),
      tag(0x0008, 0x0090), tag(0x0008, 0x0092), tag(0x0008, 0x0094), tag(0x0008, 0x1010), tag(0x0008, 0x1030),
      tag(0x0008, 0x103E), tag(0x0008, 0x1040), tag(0x0008, 0x1048), tag(0x0008, 0x1050), tag(0x0008, 0x1060),
      tag(0x0008, 0x1070), tag(0x0008, 0x1155), tag(0x0018, 0x1000), tag(0x0020, 0x000D), tag(0x0020, 0x000E),
      tag(0x0020, 0x0010), tag(0x0020, 0x0052), tag(0x0040, 0x0244), tag(0x0040, 0x0253), tag(0x0040, 0x0275),
      tag(0x0040, 0xA730)};
  return std::find(std::begin(list), std::end(list), t) != std::end(list);
}

bool long_length_vr(std::string_view vr) {
  return vr == "OB" || vr == "OD" || vr == "OF" || vr == "OL" || vr == "OV" || vr == "OW" || vr == "SQ" || vr == "UC" ||
         vr == "UN" || vr == "UR" || vr == "UT" || vr == "SV" || vr == "UV";
}

constexpr std::uint32_t undefined = 0xFFFFFFFFu;

struct Element {
  std::uint32_t tag = 0;
  std::string vr;
  ByteView value;                       ///< raw value (not for sequences / encapsulated data)
  std::vector<std::vector<Element>> items;  ///< SQ items
  std::vector<ByteView> fragments;      ///< encapsulated pixel data (first = basic offset table)
  bool encapsulated = false;
};

class Parser {
 public:
  Parser(bool explicit_vr, bool big_endian, const Context& ctx) : explicit_(explicit_vr), big_(big_endian), ctx_(ctx) {}

  Result<std::vector<Element>> dataset(util::ByteReader& reader, int depth, bool until_item_end) {
    if (depth > max_nesting) return fail(Status::Limit, "DICOM sequences are nested too deeply");
    std::vector<Element> out;
    while (reader.remaining() >= 8) {
      if (++elements_ > 2'000'000) return fail(Status::Limit, "too many DICOM elements");
      const std::size_t start = reader.position();
      const std::uint16_t g = u16(reader);
      const std::uint16_t e = u16(reader);
      const std::uint32_t t = tag(g, e);
      if (t == tag(0xFFFE, 0xE00D)) {  // item delimiter
        (void)reader.u32le();
        if (until_item_end) return out;
        continue;
      }
      if (t == tag(0xFFFE, 0xE0DD)) {
        (void)reader.u32le();
        last_was_sequence_end_ = true;
        return out;
      }
      (void)start;
      Element el;
      el.tag = t;
      std::uint32_t length = 0;
      if (explicit_ && g != 0xFFFE) {
        el.vr = std::string(reader.chars(2));
        if (el.vr.size() != 2 || !util::is_ascii_alpha(el.vr[0]) || !util::is_ascii_alpha(el.vr[1])) {
          return fail(Status::Decode, "bad DICOM VR at offset {}", reader.position());
        }
        if (long_length_vr(el.vr)) {
          (void)reader.u16le();
          length = u32(reader);
        } else {
          length = u16(reader);
        }
      } else {
        length = u32(reader);
        const DictEntry* d = lookup(t);
        el.vr = d != nullptr ? std::string(d->vr) : (e == 0x0000 ? "UL" : "UN");
        if (el.vr == "UN" && length == undefined) el.vr = "SQ";
      }
      if (!reader.ok()) return fail(Status::Decode, "DICOM element header is truncated");
      if (el.vr == "SQ") {
        CVT_TRY(sequence(reader, el, length, depth));
      } else if (t == tag(0x7FE0, 0x0010) && length == undefined) {
        CVT_TRY(encapsulated(reader, el));
      } else if (length == undefined) {
        // Undefined length UN: treat as an implicit-VR little-endian sequence.
        Parser inner(false, false, ctx_);
        el.vr = "SQ";
        CVT_TRY(inner.sequence(reader, el, length, depth));
      } else {
        if (length > reader.remaining()) return fail(Status::Decode, "DICOM element ({:04X},{:04X}) is truncated", g, e);
        el.value = reader.bytes(length);
      }
      out.push_back(std::move(el));
    }
    return out;
  }

  Result<void> sequence(util::ByteReader& reader, Element& el, std::uint32_t length, int depth) {
    if (length != undefined) {
      if (length > reader.remaining()) return fail(Status::Decode, "DICOM sequence is truncated");
      util::ByteReader body = reader.sub(length);
      return items(body, el, depth, false);
    }
    return items(reader, el, depth, true);
  }

  Result<void> items(util::ByteReader& reader, Element& el, int depth, bool undefined_length) {
    while (reader.remaining() >= 8) {
      const std::uint16_t g = u16(reader);
      const std::uint16_t e = u16(reader);
      const std::uint32_t item_length = u32(reader);
      if (g == 0xFFFE && e == 0xE0DD) return {};
      if (g != 0xFFFE || e != 0xE000) return fail(Status::Decode, "expected a DICOM item, found ({:04X},{:04X})", g, e);
      if (item_length == undefined) {
        CVT_TRY_ASSIGN(auto ds, dataset(reader, depth + 1, true));
        el.items.push_back(std::move(ds));
      } else {
        if (item_length > reader.remaining()) return fail(Status::Decode, "DICOM item is truncated");
        util::ByteReader body = reader.sub(item_length);
        CVT_TRY_ASSIGN(auto ds, dataset(body, depth + 1, false));
        el.items.push_back(std::move(ds));
      }
      if (el.items.size() > 1'000'000) return fail(Status::Limit, "too many DICOM items");
    }
    if (undefined_length) return fail(Status::Decode, "DICOM sequence has no delimiter");
    return {};
  }

  Result<void> encapsulated(util::ByteReader& reader, Element& el) {
    el.encapsulated = true;
    while (reader.remaining() >= 8) {
      const std::uint16_t g = reader.u16le();
      const std::uint16_t e = reader.u16le();
      const std::uint32_t length = reader.u32le();
      if (g == 0xFFFE && e == 0xE0DD) return {};
      if (g != 0xFFFE || e != 0xE000) return fail(Status::Decode, "bad encapsulated pixel data item");
      if (length > reader.remaining()) return fail(Status::Decode, "encapsulated pixel data is truncated");
      el.fragments.push_back(reader.bytes(length));
      if (el.fragments.size() > 1'000'000) return fail(Status::Limit, "too many pixel data fragments");
    }
    return fail(Status::Decode, "encapsulated pixel data has no delimiter");
  }

  [[nodiscard]] bool big_endian() const { return big_; }

 private:
  std::uint16_t u16(util::ByteReader& r) const { return big_ ? r.u16be() : r.u16le(); }
  std::uint32_t u32(util::ByteReader& r) const { return big_ ? r.u32be() : r.u32le(); }

  bool explicit_;
  bool big_;
  const Context& ctx_;
  std::uint64_t elements_ = 0;
  bool last_was_sequence_end_ = false;
};

struct File {
  std::vector<Element> meta;
  std::vector<Element> data;
  std::string transfer_syntax;
  bool big_endian = false;
  Bytes inflated;  // keeps deflated data sets alive
};

std::string trim_value(std::string_view text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.remove_suffix(1);
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  return util::sanitize_utf8(text);
}

Result<File> parse_file(ByteView input, const Context& ctx, std::unique_ptr<File>& holder) {
  holder = std::make_unique<File>();
  File& file = *holder;
  util::ByteReader reader(input);
  if (input.size() >= 132 && as_chars(input.subspan(128, 4)) == "DICM") {
    (void)reader.bytes(132);
  }
  // File meta information: explicit VR little endian.
  if (reader.remaining() >= 6 && reader.peek_rest()[0] == std::byte{0x02} && reader.peek_rest()[1] == std::byte{0x00}) {
    // Parse the group element by element so that group 0008 can follow in another syntax.
    while (reader.remaining() >= 8 && reader.peek_rest()[0] == std::byte{0x02} && reader.peek_rest()[1] == std::byte{0x00}) {
      Element el;
      const std::uint16_t group = reader.u16le();  // two statements: argument order is unspecified
      const std::uint16_t element = reader.u16le();
      el.tag = tag(group, element);
      el.vr = std::string(reader.chars(2));
      std::uint32_t length = long_length_vr(el.vr) ? ((void)reader.u16le(), reader.u32le()) : reader.u16le();
      if (!reader.ok() || length > reader.remaining()) return fail(Status::Decode, "DICOM file meta information is truncated");
      el.value = reader.bytes(length);
      if (el.tag == tag(0x0002, 0x0010)) file.transfer_syntax = trim_value(as_chars(el.value));
      file.meta.push_back(std::move(el));
    }
  }
  const std::string& ts = file.transfer_syntax;
  bool explicit_vr = ts != "1.2.840.10008.1.2";
  bool big = ts == "1.2.840.10008.1.2.2";
  if (ts.empty()) {
    // No meta: guess explicit vs implicit from the first element.
    const ByteView rest = reader.peek_rest();
    explicit_vr = rest.size() >= 6 && util::is_ascii_alpha(static_cast<char>(rest[4])) && util::is_ascii_alpha(static_cast<char>(rest[5]));
  }
  ByteView body = reader.peek_rest();
  if (ts == "1.2.840.10008.1.2.1.99") {
    CVT_TRY_ASSIGN(file.inflated, util::inflate_raw(body, ctx.limits.max_decompressed_bytes));
    body = file.inflated;
  }
  util::ByteReader data(body);
  Parser parser(explicit_vr, big, ctx);
  CVT_TRY_ASSIGN(file.data, parser.dataset(data, 0, false));
  file.big_endian = big;
  if (file.data.empty() && file.meta.empty()) return fail(Status::Decode, "not a DICOM file");
  return std::move(file);
}

const Element* find(const std::vector<Element>& ds, std::uint32_t t) {
  for (const Element& e : ds) {
    if (e.tag == t) return &e;
  }
  return nullptr;
}

std::vector<double> numbers(const Element* el, bool big) {
  std::vector<double> out;
  if (el == nullptr) return out;
  const std::string& vr = el->vr;
  util::ByteReader r(el->value);
  const auto order = big ? std::endian::big : std::endian::little;
  if (vr == "US" || vr == "OW") {
    while (r.remaining() >= 2) out.push_back(r.read<std::uint16_t>(order));
  } else if (vr == "SS") {
    while (r.remaining() >= 2) out.push_back(r.read<std::int16_t>(order));
  } else if (vr == "UL") {
    while (r.remaining() >= 4) out.push_back(r.read<std::uint32_t>(order));
  } else if (vr == "SL") {
    while (r.remaining() >= 4) out.push_back(r.read<std::int32_t>(order));
  } else if (vr == "FL") {
    while (r.remaining() >= 4) out.push_back(static_cast<double>(std::bit_cast<float>(r.read<std::uint32_t>(order))));
  } else if (vr == "FD") {
    while (r.remaining() >= 8) out.push_back(std::bit_cast<double>(r.read<std::uint64_t>(order)));
  } else if (vr == "DS" || vr == "IS" || vr == "UN" || vr.empty()) {
    for (const auto part : util::split(as_chars(el->value), '\\')) {
      if (const auto v = to_double(trim_value(part))) out.push_back(*v);
    }
  }
  return out;
}

double number(const std::vector<Element>& ds, std::uint32_t t, double fallback, bool big) {
  const auto v = numbers(find(ds, t), big);
  return v.empty() ? fallback : v.front();
}

std::string text(const std::vector<Element>& ds, std::uint32_t t) {
  const Element* e = find(ds, t);
  return e == nullptr ? std::string() : trim_value(as_chars(e->value));
}

// --------------------------------------------------------------------------- Data

std::string key_of(std::uint32_t t) {
  const DictEntry* d = lookup(t);
  if (d != nullptr) return std::string(d->keyword);
  return std::format("({:04X},{:04X})", t >> 16, t & 0xFFFF);
}

Json element_json(const Element& el, bool big, bool strip, int depth) {
  if (el.vr == "SQ") {
    Json items = Json::array();
    for (const auto& item : el.items) {
      Json obj = Json::object();
      for (const Element& child : item) {
        if (strip && identifying(child.tag)) continue;
        if (depth < max_nesting) obj[key_of(child.tag)] = element_json(child, big, strip, depth + 1);
      }
      items.push_back(std::move(obj));
    }
    return items;
  }
  if (el.tag == tag(0x7FE0, 0x0010)) {
    return Json::object({{"vr", el.vr}, {"encapsulated", el.encapsulated},
                         {"bytes", static_cast<std::uint64_t>(el.encapsulated ? el.fragments.size() : el.value.size())}});
  }
  const std::string& vr = el.vr;
  if (vr == "US" || vr == "SS" || vr == "UL" || vr == "SL" || vr == "FL" || vr == "FD" || vr == "DS" || vr == "IS") {
    const auto values = numbers(&el, big);
    if (values.size() == 1) return json_number(values[0]);
    Json list = Json::array();
    for (const double v : values) list.push_back(json_number(v));
    return list;
  }
  if (vr == "UN" && !el.value.empty() &&
      std::all_of(el.value.begin(), el.value.end(), [](std::byte b) {
        const auto c = static_cast<std::uint8_t>(b);
        return (c >= 32 && c < 127) || c == 0;
      })) {
    return Json(trim_value(as_chars(el.value)));  // unknown implicit-VR element that is plainly text
  }
  if (vr == "OB" || vr == "OW" || vr == "OF" || vr == "OD" || vr == "OL" || vr == "UN" || vr == "OV") {
    return Json::object({{"vr", vr}, {"bytes", static_cast<std::uint64_t>(el.value.size())}});
  }
  if (vr == "AT") {
    Json list = Json::array();
    util::ByteReader r(el.value);
    while (r.remaining() >= 4) {
      const std::uint16_t g = big ? r.u16be() : r.u16le();
      const std::uint16_t e = big ? r.u16be() : r.u16le();
      list.push_back(std::format("({:04X},{:04X})", g, e));
    }
    return list;
  }
  const std::string value = trim_value(as_chars(el.value));
  if (vr == "LT" || vr == "ST" || vr == "UT") return Json(value);
  const auto parts = util::split(value, '\\');
  if (parts.size() > 1) {
    Json list = Json::array();
    for (const auto p : parts) list.push_back(trim_value(p));
    return list;
  }
  return Json(value);
}

Result<AnyIR> read_dicom_data(ReadContext& ctx) {
  std::unique_ptr<File> holder;
  CVT_TRY_ASSIGN(File file, parse_file(ctx.input, ctx, holder));
  const bool strip = options::resolved_strip_metadata(ctx.options);
  Json meta = Json::object();
  for (const Element& el : file.meta) {
    if (strip && identifying(el.tag)) continue;
    meta[key_of(el.tag)] = element_json(el, false, strip, 0);
  }
  Json data = Json::object();
  std::size_t removed = 0;
  for (const Element& el : file.data) {
    if (strip && identifying(el.tag)) {
      ++removed;
      continue;
    }
    data[key_of(el.tag)] = element_json(el, file.big_endian, strip, 0);
  }
  if (removed > 0) ctx.warn(std::format("{} identifying DICOM elements were removed (set strip_metadata to false to keep them).", removed));
  return ir::Data{Json::object({{"file_meta", std::move(meta)}, {"dataset", std::move(data)}})};
}

// --------------------------------------------------------------------------- pixels

Result<Bytes> rle_decode_frame(ByteView frame, std::size_t segment_size, std::size_t expected_segments) {
  util::ByteReader header(frame);
  const std::uint32_t count = header.u32le();
  if (frame.size() < 64 || count == 0 || count > 15 || count != expected_segments) {
    return fail(Status::Decode, "bad RLE header ({} segments, expected {})", count, expected_segments);
  }
  std::array<std::uint32_t, 16> offsets{};
  for (std::size_t i = 0; i < 15; ++i) offsets[i] = header.u32le();
  offsets[count] = static_cast<std::uint32_t>(frame.size());
  Bytes out(segment_size * count, std::byte{0});
  for (std::uint32_t s = 0; s < count; ++s) {
    if (offsets[s] < 64 || offsets[s] > offsets[s + 1] || offsets[s + 1] > frame.size()) return fail(Status::Decode, "bad RLE segment offsets");
    const ByteView seg = frame.subspan(offsets[s], offsets[s + 1] - offsets[s]);
    std::size_t at = 0;
    std::size_t produced = 0;
    std::byte* dst = out.data() + s * segment_size;
    while (at < seg.size() && produced < segment_size) {
      const auto n = static_cast<std::int8_t>(seg[at++]);
      if (n >= 0) {
        const std::size_t copy = static_cast<std::size_t>(n) + 1;
        if (at + copy > seg.size()) return fail(Status::Decode, "RLE literal run is truncated");
        const std::size_t take = std::min(copy, segment_size - produced);
        std::memcpy(dst + produced, seg.data() + at, take);
        produced += take;
        at += copy;
      } else if (n != -128) {
        const std::size_t repeat = static_cast<std::size_t>(1 - n);
        if (at >= seg.size()) return fail(Status::Decode, "RLE replicate run is truncated");
        const std::size_t take = std::min(repeat, segment_size - produced);
        std::memset(dst + produced, static_cast<int>(seg[at]), take);
        produced += take;
        ++at;
      }
    }
    if (produced < segment_size) return fail(Status::Decode, "RLE segment is short");
  }
  return out;
}

struct PixelInfo {
  std::uint32_t rows = 0, columns = 0, frames = 1, samples = 1, bits_allocated = 8, bits_stored = 8;
  bool is_signed = false;
  std::uint32_t planar = 0;
  std::string photometric = "MONOCHROME2";
  double slope = 1, intercept = 0;
};

/// Samples of one frame (interleaved), as doubles after rescale.
Result<std::vector<double>> unpack_frame(ByteView raw, const PixelInfo& p, bool big) {
  const std::size_t pixels = std::size_t{p.rows} * p.columns;
  const std::size_t count = pixels * p.samples;
  std::vector<double> out(count);
  if (p.bits_allocated == 1) {
    if (raw.size() * 8 < count) return fail(Status::Decode, "DICOM bit-packed pixel data is truncated");
    for (std::size_t i = 0; i < count; ++i) out[i] = (static_cast<std::uint8_t>(raw[i / 8]) >> (i % 8)) & 1;
    return out;
  }
  const std::size_t bytes = p.bits_allocated / 8;
  if (bytes == 0 || bytes > 4 || raw.size() < count * bytes) return fail(Status::Decode, "DICOM pixel data is truncated");
  const std::uint32_t mask_bits = std::min<std::uint32_t>(p.bits_stored, p.bits_allocated);
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t v = 0;
    for (std::size_t b = 0; b < bytes; ++b) {
      const std::uint32_t byte = static_cast<std::uint8_t>(raw[i * bytes + b]);
      v |= big ? byte << (8 * (bytes - 1 - b)) : byte << (8 * b);
    }
    if (mask_bits < 32) v &= (1u << mask_bits) - 1;
    double value = v;
    if (p.is_signed && mask_bits > 0 && (v >> (mask_bits - 1)) & 1) value = static_cast<double>(static_cast<std::int64_t>(v) - (std::int64_t{1} << mask_bits));
    out[i] = value;
  }
  if (p.planar == 1 && p.samples > 1) {
    std::vector<double> interleaved(count);
    for (std::size_t s = 0; s < p.samples; ++s) {
      for (std::size_t i = 0; i < pixels; ++i) interleaved[i * p.samples + s] = out[s * pixels + i];
    }
    out = std::move(interleaved);
  }
  return out;
}

std::array<std::uint8_t, 3> ybr_to_rgb(double y, double cb, double cr) {
  const double r = y + 1.402 * (cr - 128);
  const double g = y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128);
  const double b = y + 1.772 * (cb - 128);
  const auto c = [](double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
  return {c(r), c(g), c(b)};
}

Result<AnyIR> read_dicom_image(ReadContext& ctx) {
  std::unique_ptr<File> holder;
  CVT_TRY_ASSIGN(File file, parse_file(ctx.input, ctx, holder));
  const auto& ds = file.data;
  const bool big = file.big_endian;
  const Element* pixel = find(ds, tag(0x7FE0, 0x0010));
  if (pixel == nullptr) return fail(Status::Decode, "the DICOM file has no pixel data");
  PixelInfo p;
  p.rows = static_cast<std::uint32_t>(number(ds, tag(0x0028, 0x0010), 0, big));
  p.columns = static_cast<std::uint32_t>(number(ds, tag(0x0028, 0x0011), 0, big));
  p.frames = static_cast<std::uint32_t>(std::max(1.0, number(ds, tag(0x0028, 0x0008), 1, big)));
  p.samples = static_cast<std::uint32_t>(number(ds, tag(0x0028, 0x0002), 1, big));
  p.bits_allocated = static_cast<std::uint32_t>(number(ds, tag(0x0028, 0x0100), 8, big));
  p.bits_stored = static_cast<std::uint32_t>(number(ds, tag(0x0028, 0x0101), p.bits_allocated, big));
  p.is_signed = number(ds, tag(0x0028, 0x0103), 0, big) != 0;
  p.planar = static_cast<std::uint32_t>(number(ds, tag(0x0028, 0x0006), 0, big));
  p.photometric = util::to_upper(text(ds, tag(0x0028, 0x0004)));
  if (p.photometric.empty()) p.photometric = p.samples == 3 ? "RGB" : "MONOCHROME2";
  p.slope = number(ds, tag(0x0028, 0x1053), 1, big);
  p.intercept = number(ds, tag(0x0028, 0x1052), 0, big);
  if (p.rows == 0 || p.columns == 0) return fail(Status::Decode, "DICOM image has no rows or columns");
  if (p.samples != 1 && p.samples != 3) return fail(Status::Decode, "unsupported samples per pixel {}", p.samples);
  if (p.bits_allocated != 1 && p.bits_allocated != 8 && p.bits_allocated != 16 && p.bits_allocated != 32) {
    return fail(Status::Decode, "unsupported bits allocated {}", p.bits_allocated);
  }
  CVT_TRY(ctx.limits.check_pixels(p.columns, p.rows, p.frames));
  const std::size_t pixels = std::size_t{p.rows} * p.columns;
  const std::string& ts = file.transfer_syntax;
  // Collect each frame's samples (after rescale for monochrome).
  std::vector<std::vector<double>> frames;
  std::vector<ir::Frame> decoded_frames;  // frames decoded by another codec
  if (!pixel->encapsulated) {
    const std::size_t frame_bytes = p.bits_allocated == 1 ? (pixels * p.samples + 7) / 8 : pixels * p.samples * (p.bits_allocated / 8);
    if (p.photometric == "YBR_FULL_422") {
      const std::size_t bytes422 = pixels * 2;
      for (std::uint32_t f = 0; f < p.frames; ++f) {
        if (pixel->value.size() < (f + 1) * bytes422) return fail(Status::Decode, "DICOM pixel data is truncated");
        const ByteView raw = pixel->value.subspan(f * bytes422, bytes422);
        std::vector<double> rgb(pixels * 3);
        for (std::size_t i = 0; i + 1 < pixels + 1 && i / 2 * 4 + 3 < raw.size(); i += 2) {
          const double y1 = static_cast<std::uint8_t>(raw[i / 2 * 4]), y2 = static_cast<std::uint8_t>(raw[i / 2 * 4 + 1]);
          const double cb = static_cast<std::uint8_t>(raw[i / 2 * 4 + 2]), cr = static_cast<std::uint8_t>(raw[i / 2 * 4 + 3]);
          const auto a = ybr_to_rgb(y1, cb, cr);
          const auto b = ybr_to_rgb(y2, cb, cr);
          for (int c = 0; c < 3; ++c) {
            rgb[i * 3 + static_cast<std::size_t>(c)] = a[static_cast<std::size_t>(c)];
            if (i + 1 < pixels) rgb[(i + 1) * 3 + static_cast<std::size_t>(c)] = b[static_cast<std::size_t>(c)];
          }
        }
        frames.push_back(std::move(rgb));
      }
      p.photometric = "RGB";
    } else {
      for (std::uint32_t f = 0; f < p.frames; ++f) {
        CVT_TRY(ctx.checkpoint(static_cast<double>(f) / p.frames));
        if (pixel->value.size() < (std::size_t{f} + 1) * frame_bytes) return fail(Status::Decode, "DICOM pixel data is truncated");
        CVT_TRY_ASSIGN(auto samples, unpack_frame(pixel->value.subspan(f * frame_bytes, frame_bytes), p, big));
        frames.push_back(std::move(samples));
      }
    }
  } else {
    // Encapsulated: one fragment per frame (fragments after the offset table), else all fragments form one frame.
    std::vector<Bytes> frame_data;
    const auto& frags = pixel->fragments;
    if (frags.size() < 2) return fail(Status::Decode, "encapsulated DICOM pixel data has no fragments");
    if (frags.size() - 1 == p.frames) {
      for (std::size_t i = 1; i < frags.size(); ++i) frame_data.emplace_back(frags[i].begin(), frags[i].end());
    } else if (p.frames == 1) {
      Bytes all;
      for (std::size_t i = 1; i < frags.size(); ++i) append(all, frags[i]);
      frame_data.push_back(std::move(all));
    } else {
      // Use the basic offset table.
      util::ByteReader table(frags[0]);
      std::vector<std::uint32_t> offsets;
      while (table.remaining() >= 4) offsets.push_back(table.u32le());
      Bytes all;
      std::vector<std::size_t> starts;
      std::size_t position = 0;
      for (std::size_t i = 1; i < frags.size(); ++i) {
        starts.push_back(position);
        append(all, frags[i]);
        position += frags[i].size() + 8;
      }
      if (offsets.size() != p.frames) return fail(Status::Decode, "cannot split encapsulated pixel data into {} frames", p.frames);
      // Map item-relative offsets to concatenated data offsets.
      std::vector<std::size_t> data_starts;
      for (const auto o : offsets) {
        std::size_t acc = 0;
        std::size_t item = 0;
        while (item < starts.size() && starts[item] < o) acc += frags[++item].size();
        data_starts.push_back(acc);
      }
      for (std::size_t f = 0; f < data_starts.size(); ++f) {
        const std::size_t end = f + 1 < data_starts.size() ? data_starts[f + 1] : all.size();
        if (data_starts[f] > end || end > all.size()) return fail(Status::Decode, "bad DICOM offset table");
        frame_data.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(data_starts[f]), all.begin() + static_cast<std::ptrdiff_t>(end));
      }
    }
    if (ts == "1.2.840.10008.1.2.5") {
      const std::size_t bytes = p.bits_allocated / 8;
      for (const Bytes& fd : frame_data) {
        CVT_TRY_ASSIGN(const Bytes planes, rle_decode_frame(fd, pixels, bytes * p.samples));
        // Segments: for each sample, the bytes from most to least significant.
        Bytes raw(pixels * p.samples * bytes);
        for (std::size_t s = 0; s < p.samples; ++s) {
          for (std::size_t b = 0; b < bytes; ++b) {
            const std::byte* seg = planes.data() + (s * bytes + b) * pixels;
            for (std::size_t i = 0; i < pixels; ++i) raw[(i * p.samples + s) * bytes + (bytes - 1 - b)] = seg[i];
          }
        }
        PixelInfo q = p;
        q.planar = 0;
        CVT_TRY_ASSIGN(auto samples, unpack_frame(raw, q, false));
        frames.push_back(std::move(samples));
      }
    } else {
      // Hand the frames to an image codec of the registry.
      std::string format;
      if (ts.starts_with("1.2.840.10008.1.2.4.5") || ts.starts_with("1.2.840.10008.1.2.4.7") ||
          ts == "1.2.840.10008.1.2.4.57" || ts == "1.2.840.10008.1.2.4.70") {
        format = "jpg";
      } else if (ts == "1.2.840.10008.1.2.4.80" || ts == "1.2.840.10008.1.2.4.81") {
        format = "jls";
      } else if (ts == "1.2.840.10008.1.2.4.90" || ts == "1.2.840.10008.1.2.4.91" || ts.starts_with("1.2.840.10008.1.2.4.20")) {
        format = "jp2";
      }
      const Codec* codec = nullptr;
      if (!format.empty() && ctx.registry != nullptr) {
        for (const Codec* c : ctx.registry->codecs_for(format)) {
          if (c->kind == Kind::Image && c->read) {
            codec = c;
            break;
          }
        }
      }
      if (codec == nullptr) {
        return fail(Status::Decode, "DICOM transfer syntax {} needs a {} decoder, which is not available in this build", ts,
                    format.empty() ? "matching" : format);
      }
      for (const Bytes& fd : frame_data) {
        ReadContext sub;
        sub.limits = ctx.limits;
        sub.registry = ctx.registry;
        sub.input = fd;
        sub.format_id = format;
        sub.warnings = ctx.warnings;
        CVT_TRY_ASSIGN(AnyIR value, codec->read(sub));
        if (!value.is<ir::Image>()) return fail(Status::Decode, "the {} decoder returned no image", format);
        ir::Image img = std::move(value).get<ir::Image>();
        if (!img.valid()) return fail(Status::Decode, "the {} decoder returned an empty image", format);
        decoded_frames.push_back(std::move(img.frames.front()));
      }
      ctx.warn("Compressed DICOM frames were decoded to 8 bits per sample.");
    }
  }
  ir::Image image;
  if (!decoded_frames.empty()) {
    for (auto& f : decoded_frames) {
      if (p.photometric == "MONOCHROME1") {
        for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
          for (int c = 0; c < 3; ++c) f.rgba[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(255 - f.rgba[i + static_cast<std::size_t>(c)]);
        }
      }
      image.frames.push_back(std::move(f));
    }
    return image;
  }
  if (p.samples == 3 || p.photometric == "PALETTE COLOR") {
    // Colour: RGB, YBR_FULL or palette.
    std::vector<std::vector<double>> luts(3);
    std::vector<double> lut_first(3, 0);
    if (p.photometric == "PALETTE COLOR") {
      for (int c = 0; c < 3; ++c) {
        const auto descriptor = numbers(find(ds, tag(0x0028, static_cast<std::uint16_t>(0x1101 + c))), big);
        const Element* data_el = find(ds, tag(0x0028, static_cast<std::uint16_t>(0x1201 + c)));
        if (descriptor.size() < 3 || data_el == nullptr) return fail(Status::Decode, "palette colour image without a lookup table");
        Element as_ow = *data_el;
        as_ow.vr = "OW";
        luts[static_cast<std::size_t>(c)] = numbers(&as_ow, big);
        lut_first[static_cast<std::size_t>(c)] = descriptor[1];
        if (descriptor[2] == 8) {
          for (double& v : luts[static_cast<std::size_t>(c)]) v = std::fmod(v, 256.0) * 257.0;  // 8-bit entries
        }
      }
    }
    for (const auto& samples : frames) {
      ir::Frame frame = ir::Frame::filled(p.columns, p.rows, {0, 0, 0, 255});
      const double max_value = std::ldexp(1.0, static_cast<int>(p.bits_stored)) - 1;
      for (std::size_t i = 0; i < pixels; ++i) {
        std::array<double, 3> rgb{};
        if (p.photometric == "PALETTE COLOR") {
          const double index = samples[i];
          for (int c = 0; c < 3; ++c) {
            const auto& lut = luts[static_cast<std::size_t>(c)];
            const auto k = static_cast<std::ptrdiff_t>(std::clamp(index - lut_first[static_cast<std::size_t>(c)], 0.0,
                                                                  static_cast<double>(lut.empty() ? 0 : lut.size() - 1)));
            rgb[static_cast<std::size_t>(c)] = lut.empty() ? 0 : lut[static_cast<std::size_t>(k)] / 65535.0 * 255.0;
          }
        } else {
          for (int c = 0; c < 3; ++c) rgb[static_cast<std::size_t>(c)] = samples[i * 3 + static_cast<std::size_t>(c)] / std::max(1.0, max_value) * 255.0;
          if (p.photometric == "YBR_FULL") {
            const auto conv = ybr_to_rgb(rgb[0], rgb[1], rgb[2]);
            rgb = {static_cast<double>(conv[0]), static_cast<double>(conv[1]), static_cast<double>(conv[2])};
          }
        }
        for (int c = 0; c < 3; ++c) {
          frame.rgba[i * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::clamp(std::lround(rgb[static_cast<std::size_t>(c)]), 0L, 255L));
        }
      }
      image.frames.push_back(std::move(frame));
    }
    return image;
  }
  // Monochrome: modality rescale, then the VOI window.
  imaging::Window window = imaging::window_from_options(ctx.options);
  window.invert = p.photometric == "MONOCHROME1";
  const std::string voi = ctx.options.get_string("dicom_window", "file");
  if (!window.low && voi == "file") {
    const auto centers = numbers(find(ds, tag(0x0028, 0x1050)), big);
    const auto widths = numbers(find(ds, tag(0x0028, 0x1051)), big);
    if (!centers.empty() && !widths.empty() && widths[0] > 0) {
      // PS3.3 C.11.2.1.2: linear window.
      window.low = centers[0] - 0.5 - (widths[0] - 1) / 2;
      window.high = centers[0] - 0.5 + (widths[0] - 1) / 2;
    }
  }
  std::vector<double> all;
  all.reserve(pixels * frames.size());
  for (auto& samples : frames) {
    for (double& v : samples) v = v * p.slope + p.intercept;
    all.insert(all.end(), samples.begin(), samples.end());
  }
  const auto range = imaging::value_range(all, window);
  for (const auto& samples : frames) image.frames.push_back(imaging::render(samples, p.columns, p.rows, window, range));
  return image;
}

// --------------------------------------------------------------------------- writing

/// "2.25." + the decimal value of 128 bits taken from a hash (PS3.5 B.2).
std::string uid_from(ByteView data, std::string_view salt) {
  util::Sha256 hasher;
  hasher.update(data);
  hasher.update(as_bytes(salt));
  const auto digest = hasher.finish();
  std::array<std::uint32_t, 4> words_le{};
  for (std::size_t i = 0; i < 16; ++i) words_le[3 - i / 4] |= std::uint32_t{static_cast<std::uint8_t>(digest[i])} << (8 * (3 - i % 4));
  std::string digits;
  auto value = words_le;
  while (std::any_of(value.begin(), value.end(), [](std::uint32_t w) { return w != 0; })) {
    std::uint64_t rem = 0;
    for (std::size_t i = 4; i-- > 0;) {
      const std::uint64_t cur = (rem << 32) | value[i];
      value[i] = static_cast<std::uint32_t>(cur / 10);
      rem = cur % 10;
    }
    digits.push_back(static_cast<char>('0' + rem));
  }
  if (digits.empty()) digits = "0";
  std::reverse(digits.begin(), digits.end());
  return "2.25." + digits;
}

void put(util::ByteWriter& out, std::uint16_t g, std::uint16_t e, std::string_view vr, ByteView value) {
  out.u16le(g).u16le(e).text(vr);
  if (long_length_vr(vr)) {
    out.u16le(0).u32le(static_cast<std::uint32_t>(value.size()));
  } else {
    out.u16le(static_cast<std::uint16_t>(value.size()));
  }
  out.bytes(value);
}

void put_text(util::ByteWriter& out, std::uint16_t g, std::uint16_t e, std::string_view vr, std::string value) {
  if (value.size() % 2 == 1) value.push_back(vr == "UI" ? '\0' : ' ');
  put(out, g, e, vr, as_bytes(value));
}

void put_us(util::ByteWriter& out, std::uint16_t g, std::uint16_t e, std::uint16_t v) {
  util::ByteWriter b(2);
  b.u16le(v);
  put(out, g, e, "US", b.view());
}

Result<Bytes> write_dicom(const AnyIR& value, WriteContext& ctx) {
  const ir::Image& image = value.get<ir::Image>();
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  const std::uint32_t w = image.width(), h = image.height();
  if (w > 65535 || h > 65535) return fail(Status::Encode, "DICOM images are limited to 65535 x 65535 pixels");
  bool grey = true;
  for (const auto& f : image.frames) grey = grey && imaging::is_grey(f);
  const bool multi = image.frames.size() > 1;
  bool transparency = false;
  Bytes pixels;
  pixels.reserve(std::size_t{w} * h * (grey ? 1 : 3) * image.frames.size());
  for (const auto& f : image.frames) {
    for (std::size_t i = 0; i < f.rgba.size(); i += 4) {
      const std::uint32_t a = f.rgba[i + 3];
      transparency = transparency || a != 255;
      for (int c = 0; c < (grey ? 1 : 3); ++c) {
        const std::uint32_t v = (f.rgba[i + static_cast<std::size_t>(c)] * a + 0 * (255 - a) + 127) / 255;  // over black
        pixels.push_back(static_cast<std::byte>(v));
      }
    }
  }
  if (transparency) ctx.warn("DICOM has no transparency; transparent areas were blended onto black.");
  if (pixels.size() % 2 == 1) pixels.push_back(std::byte{0});
  const std::string sop_class = !multi ? "1.2.840.10008.5.1.4.1.1.7"
                                : grey ? "1.2.840.10008.5.1.4.1.1.7.2"
                                       : "1.2.840.10008.5.1.4.1.1.7.4";
  const std::string study_uid = uid_from(pixels, "study");
  const std::string series_uid = uid_from(pixels, "series");
  const std::string instance_uid = uid_from(pixels, "instance");
  util::ByteWriter ds(pixels.size() + 1024);
  put_text(ds, 0x0008, 0x0016, "UI", sop_class);
  put_text(ds, 0x0008, 0x0018, "UI", instance_uid);
  put_text(ds, 0x0008, 0x0020, "DA", "");
  put_text(ds, 0x0008, 0x0030, "TM", "");
  put_text(ds, 0x0008, 0x0050, "SH", "");
  put_text(ds, 0x0008, 0x0060, "CS", "OT");
  put_text(ds, 0x0008, 0x0064, "CS", "WSD");
  put_text(ds, 0x0008, 0x0090, "PN", "");
  put_text(ds, 0x0010, 0x0010, "PN", "");
  put_text(ds, 0x0010, 0x0020, "LO", "");
  put_text(ds, 0x0010, 0x0030, "DA", "");
  put_text(ds, 0x0010, 0x0040, "CS", "");
  put_text(ds, 0x0020, 0x000D, "UI", study_uid);
  put_text(ds, 0x0020, 0x000E, "UI", series_uid);
  put_text(ds, 0x0020, 0x0010, "SH", "");
  put_text(ds, 0x0020, 0x0011, "IS", "1");
  put_text(ds, 0x0020, 0x0013, "IS", "1");
  put_text(ds, 0x0020, 0x0020, "CS", "");
  put_us(ds, 0x0028, 0x0002, grey ? 1 : 3);
  put_text(ds, 0x0028, 0x0004, "CS", grey ? "MONOCHROME2" : "RGB");
  if (!grey) put_us(ds, 0x0028, 0x0006, 0);
  if (multi) {
    put_text(ds, 0x0028, 0x0008, "IS", std::to_string(image.frames.size()));
    util::ByteWriter at(4);
    at.u16le(0x0018).u16le(0x2002);  // Frame Increment Pointer -> Frame Label Vector (type 1C requirement satisfied minimally)
    put(ds, 0x0028, 0x0009, "AT", at.view());
  }
  put_us(ds, 0x0028, 0x0010, static_cast<std::uint16_t>(h));
  put_us(ds, 0x0028, 0x0011, static_cast<std::uint16_t>(w));
  put_us(ds, 0x0028, 0x0100, 8);
  put_us(ds, 0x0028, 0x0101, 8);
  put_us(ds, 0x0028, 0x0102, 7);
  put_us(ds, 0x0028, 0x0103, 0);
  put(ds, 0x7FE0, 0x0010, "OB", pixels);
  // File meta information.
  util::ByteWriter meta(256);
  const std::uint8_t version[] = {0, 1};
  put(meta, 0x0002, 0x0001, "OB", as_bytes(std::span<const std::uint8_t>(version)));
  put_text(meta, 0x0002, 0x0002, "UI", sop_class);
  put_text(meta, 0x0002, 0x0003, "UI", instance_uid);
  put_text(meta, 0x0002, 0x0010, "UI", "1.2.840.10008.1.2.1");
  put_text(meta, 0x0002, 0x0012, "UI", "2.25.229458102633745358474618040612931218049");
  util::ByteWriter out(ds.size() + meta.size() + 160);
  out.zeros(128).text("DICM");
  util::ByteWriter length(4);
  length.u32le(static_cast<std::uint32_t>(meta.size()));
  put(out, 0x0002, 0x0000, "UL", length.view());
  out.bytes(meta.view());
  out.bytes(ds.view());
  return out.take();
}

int sniff_dicom(ByteView data, std::string_view name) {
  if (data.size() >= 132 && as_chars(data.subspan(128, 4)) == "DICM") return 97;
  if (data.size() >= 8 && (has_extension(name, "dcm") || has_extension(name, "dicom"))) {
    const auto g = static_cast<std::uint8_t>(data[0]) | (static_cast<std::uint8_t>(data[1]) << 8);
    if (g == 0x0008 || g == 0x0002) return 80;
  }
  return 0;
}

}  // namespace

void register_dicom(Registry& registry) {
  add_format(registry, {.id = "dcm",
                        .name = "DICOM image",
                        .description = "Digital Imaging and Communications in Medicine file.",
                        .category = Category::Image,
                        .extensions = {"dcm", "dicom", "dic"},
                        .mime = "application/dicom",
                        .popularity = 30,
                        .signatures = {Signature::at(128, "'DICM'")},
                        .sniff = sniff_dicom});
  std::vector<OptionSpec> image_read = imaging::window_options("linear", 0.0);
  image_read.push_back(OptionSpec::choice("dicom_window", "Window", "file", {"file", "auto"},
                                          "Use the window centre/width stored in the file, or the full data range."));
  add_codec(registry, "dcm", Kind::Image, read_dicom_image, write_dicom, image_read, {}, 20, 20, true);
  add_codec(registry, "dcm", Kind::Data, read_dicom_data, nullptr, {options::strip_metadata()}, {}, 10, 10);
}

}  // namespace cvt::science
