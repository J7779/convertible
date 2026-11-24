// Convertible core - office family: Windows locale ids (LCID).
//
// Help files and Microsoft Reader books name their locale by LCID; the
// ANSI code page of that locale decodes their text and the language tag
// goes into the document metadata.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string_view>

namespace cvt::office::lcid {

/// The Windows ANSI code page of a locale (1252 when unknown).
[[nodiscard]] constexpr int code_page(std::uint32_t lcid) noexcept {
  const std::uint32_t full = lcid & 0xFFFFu;
  switch (full) {
    case 0x0404: case 0x0C04: case 0x1404: return 950;  // Chinese (Traditional)
    case 0x081A: case 0x141A: return 1250;              // Serbian / Bosnian (Latin)
    case 0x0C1A: case 0x201A: return 1251;              // Serbian / Bosnian (Cyrillic)
    case 0x0443: return 1254;                           // Uzbek (Latin)
    case 0x0843: return 1251;                           // Uzbek (Cyrillic)
    case 0x042C: return 1254;                           // Azeri (Latin)
    case 0x082C: return 1251;                           // Azeri (Cyrillic)
    default: break;
  }
  switch (full & 0x3FFu) {
    case 0x01: case 0x20: case 0x29: return 1256;                                          // Arabic, Urdu, Farsi
    case 0x02: case 0x19: case 0x22: case 0x23: case 0x2F: case 0x3F: case 0x40: case 0x44:
    case 0x50: return 1251;                                                                 // Cyrillic
    case 0x04: return 936;                                                                  // Chinese (Simplified)
    case 0x05: case 0x0E: case 0x15: case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x24: return 1250;  // Central European
    case 0x08: return 1253;                                                                 // Greek
    case 0x0D: return 1255;                                                                 // Hebrew
    case 0x11: return 932;                                                                  // Japanese
    case 0x12: return 949;                                                                  // Korean
    case 0x1E: return 874;                                                                  // Thai
    case 0x1F: return 1254;                                                                 // Turkish
    case 0x25: case 0x26: case 0x27: return 1257;                                           // Baltic
    case 0x2A: return 1258;                                                                 // Vietnamese
    default: return 1252;
  }
}

/// The charset label the document family's HTML reader understands for a
/// code page, or empty when it has no decoder for it.
[[nodiscard]] constexpr std::string_view html_charset(int code_page) noexcept {
  switch (code_page) {
    case 1250: return "windows-1250";
    case 1251: return "windows-1251";
    case 1252: return "windows-1252";
    case 1253: return "windows-1253";
    case 1254: return "windows-1254";
    case 1255: return "windows-1255";
    case 1256: return "windows-1256";
    case 1257: return "windows-1257";
    case 1258: return "windows-1258";
    default: return {};
  }
}

/// BCP 47 tag of a locale ("en-US"), empty when unknown.
[[nodiscard]] constexpr std::string_view language_tag(std::uint32_t lcid) noexcept {
  switch (lcid & 0xFFFFu) {
    case 0x0401: return "ar-SA";
    case 0x0402: return "bg-BG";
    case 0x0403: return "ca-ES";
    case 0x0404: return "zh-TW";
    case 0x0405: return "cs-CZ";
    case 0x0406: return "da-DK";
    case 0x0407: return "de-DE";
    case 0x0408: return "el-GR";
    case 0x0409: return "en-US";
    case 0x040A: return "es-ES";
    case 0x040B: return "fi-FI";
    case 0x040C: return "fr-FR";
    case 0x040D: return "he-IL";
    case 0x040E: return "hu-HU";
    case 0x040F: return "is-IS";
    case 0x0410: return "it-IT";
    case 0x0411: return "ja-JP";
    case 0x0412: return "ko-KR";
    case 0x0413: return "nl-NL";
    case 0x0414: return "nb-NO";
    case 0x0415: return "pl-PL";
    case 0x0416: return "pt-BR";
    case 0x0418: return "ro-RO";
    case 0x0419: return "ru-RU";
    case 0x041A: return "hr-HR";
    case 0x041B: return "sk-SK";
    case 0x041D: return "sv-SE";
    case 0x041E: return "th-TH";
    case 0x041F: return "tr-TR";
    case 0x0421: return "id-ID";
    case 0x0422: return "uk-UA";
    case 0x0423: return "be-BY";
    case 0x0424: return "sl-SI";
    case 0x0425: return "et-EE";
    case 0x0426: return "lv-LV";
    case 0x0427: return "lt-LT";
    case 0x0429: return "fa-IR";
    case 0x042A: return "vi-VN";
    case 0x0804: return "zh-CN";
    case 0x0807: return "de-CH";
    case 0x0809: return "en-GB";
    case 0x080A: return "es-MX";
    case 0x080C: return "fr-BE";
    case 0x0813: return "nl-BE";
    case 0x0816: return "pt-PT";
    case 0x0C07: return "de-AT";
    case 0x0C09: return "en-AU";
    case 0x0C0A: return "es-ES";
    case 0x0C0C: return "fr-CA";
    case 0x1009: return "en-CA";
    default: return {};
  }
}

}  // namespace cvt::office::lcid
