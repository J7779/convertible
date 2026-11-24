// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Unit tests of the structured content sniffers on small hand-built inputs:
// each case states the bytes a specification prescribes and the id the
// classifier must report, plus the look-alikes it must not confuse.
#include <cstdint>
#include <string>
#include <string_view>

#include "engine/sniffers.hpp"
#include "support/test_support.hpp"
#include "util/deflate.hpp"

using namespace cvt;
using namespace std::string_literals;
using namespace std::string_view_literals;
using sniff::Family;

namespace {

std::string id_of(Family family, std::string_view data, std::string_view name = {}, std::string_view wanted = {}) {
  const sniff::Verdict verdict = sniff::classify(family, as_bytes(data), name, wanted);
  return verdict.id.empty() ? std::string("-") : std::string(verdict.id);
}

std::string u32be(std::uint32_t v) {
  return {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}
std::string u32le(std::uint32_t v) {
  return {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
}
std::string u16le(std::uint32_t v) { return {static_cast<char>(v), static_cast<char>(v >> 8)}; }

std::string ftyp(std::string_view major, std::string_view compatible = "isom") {
  const std::string body = std::string(major) + "\0\0\0\0"s + std::string(compatible);
  return u32be(static_cast<std::uint32_t>(8 + body.size())) + "ftyp" + body;
}

// Every prefix of `data` through every classifier: nothing may crash or read out of bounds.
void all_prefixes(std::string_view data, std::string_view name) {
  for (std::size_t cut = 0; cut <= data.size(); ++cut) {
    for (int f = 0; f <= static_cast<int>(Family::Misc); ++f) {
      for (const std::string_view wanted : {"zip"sv, "docx"sv, "apng"sv, "ppm"sv, "ico"sv, "stl"sv, "mod"sv, "torrent"sv}) {
        (void)sniff::classify(static_cast<Family>(f), as_bytes(data.substr(0, cut)), name, wanted);
      }
    }
  }
}

}  // namespace

TEST_CASE("sniffers: empty input identifies nothing") {
  for (int f = 0; f <= static_cast<int>(Family::Misc); ++f) {
    const sniff::Verdict v = sniff::classify(static_cast<Family>(f), {}, "x.zip", "zip");
    CHECK(v.id.empty());
    CHECK_FALSE(v.container);
  }
}

TEST_CASE("sniffers: ZIP packages by member names") {
  const auto zip = [](std::string_view name) {
    return "PK\x03\x04\x14\0\0\0\0\0"s + std::string(16, '\0') + u16le(static_cast<std::uint32_t>(name.size())) + "\0\0"s +
           std::string(name);
  };
  CHECK(id_of(Family::Zip, zip("word/document.xml"), "", "docx") == "docx");
  CHECK(id_of(Family::Zip, zip("xl/workbook.xml"), "", "xlsx") == "xlsx");
  CHECK(id_of(Family::Zip, zip("xl/workbook.bin"), "", "xlsb") == "xlsb");
  CHECK(id_of(Family::Zip, zip("ppt/presentation.xml"), "deck.potx", "potx") == "potx");
  CHECK(id_of(Family::Zip, zip("AndroidManifest.xml"), "", "apk") == "apk");
  CHECK(id_of(Family::Zip, zip("BundleConfig.pb"), "", "aab") == "aab");
  CHECK(id_of(Family::Zip, zip("META-INF/MANIFEST.MF"), "", "jar") == "jar");
  CHECK(id_of(Family::Zip, zip("doc.kml"), "", "kmz") == "kmz");
  CHECK(id_of(Family::Zip, zip("3D/3dmodel.model"), "", "3mf") == "3mf");
  CHECK(id_of(Family::Zip, zip("scene.usdc"), "", "usdz") == "usdz");
  CHECK(id_of(Family::Zip, zip("Payload/App.app/Info.plist"), "", "ipa") == "ipa");
  CHECK(id_of(Family::Zip, zip("pkg-1.0.dist-info/WHEEL"), "", "whl") == "whl");
  CHECK(id_of(Family::Zip, zip("hello.txt"), "", "zip") == "-");  // a plain ZIP: no specific format
  CHECK(sniff::classify(Family::Zip, as_bytes(zip("hello.txt")), "", "cbz").container);
  // The mimetype member of OpenDocument and EPUB, stored first.
  const std::string mime = "application/vnd.oasis.opendocument.spreadsheet";
  const std::string ods = "PK\x03\x04\x14\0\0\0\0\0"s + std::string(8, '\0') + u32le(static_cast<std::uint32_t>(mime.size())) +
                          u32le(static_cast<std::uint32_t>(mime.size())) + "\x08\0\0\0"s + "mimetype" + mime;
  CHECK(id_of(Family::Zip, ods, "", "ods") == "ods");
  all_prefixes(ods, "x.ods");
}

TEST_CASE("sniffers: ISO base media by brand") {
  CHECK(id_of(Family::Bmff, ftyp("isom")) == "mp4");
  CHECK(sniff::classify(Family::Bmff, as_bytes(ftyp("isom")), "", "mp4").base);
  CHECK(id_of(Family::Bmff, ftyp("isom"), "song.m4a") == "m4a");  // a structural brand: the name refines it
  CHECK(id_of(Family::Bmff, ftyp("M4A ")) == "m4a");
  CHECK(id_of(Family::Bmff, ftyp("M4A "), "clip.mov") == "m4a");  // a specific brand beats the name
  CHECK(id_of(Family::Bmff, ftyp("M4V ")) == "m4v");
  CHECK(id_of(Family::Bmff, ftyp("qt  ")) == "mov");
  CHECK(id_of(Family::Bmff, ftyp("3gp5")) == "3gp");
  CHECK(id_of(Family::Bmff, ftyp("3g2a")) == "3g2");
  CHECK(id_of(Family::Bmff, ftyp("heic", "mif1heic")) == "heic");
  CHECK(id_of(Family::Bmff, ftyp("mif1", "mif1avif")) == "avif");
  CHECK(id_of(Family::Bmff, ftyp("avif")) == "avif");
  CHECK(id_of(Family::Bmff, ftyp("crx ", "crx isom")) == "cr3");
  CHECK(id_of(Family::Bmff, ftyp("f4v ")) == "f4v");
  CHECK(id_of(Family::Bmff, ftyp("isml")) == "ismv");
  CHECK(id_of(Family::Bmff, "\0\0\0\x18styp" "msdh\0\0\0\0msdhmsix"s) == "m4s");
  CHECK(id_of(Family::Bmff, "\0\0\0\x0CjP  \r\n\x87\n"s + ftyp("jpx ", "jpx jp2 ")) == "jpx");
  CHECK(id_of(Family::Bmff, "\0\0\0\x08wide\0\0\0\x10mdat"s + std::string(8, 'x')) == "mov");
  CHECK(id_of(Family::Bmff, "\0\0\0\x08" "abcd"s) == "-");
  all_prefixes(ftyp("mif1", "mif1avif"), "x.avif");
}

TEST_CASE("sniffers: Matroska DocType and track types") {
  const std::string header = "\x1A\x45\xDF\xA3\x8B\x42\x82\x88matroska"s;
  CHECK(id_of(Family::Ebml, header) == "mkv");
  CHECK(id_of(Family::Ebml, "\x1A\x45\xDF\xA3\x87\x42\x82\x84webm"s) == "webm");
  // Segment (unknown size) > Tracks > TrackEntry > TrackType = 2 (audio).
  const std::string audio = header + "\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF"s + "\x16\x54\xAE\x6B\x85\xAE\x83\x83\x81\x02"s;
  CHECK(id_of(Family::Ebml, audio) == "mka");
  const std::string subtitles = header + "\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF"s + "\x16\x54\xAE\x6B\x85\xAE\x83\x83\x81\x11"s;
  CHECK(id_of(Family::Ebml, subtitles) == "mks");
  CHECK(id_of(Family::Ebml, header, "film.mk3d") == "mk3d");
  all_prefixes(audio, "x.mka");
}

TEST_CASE("sniffers: Ogg by the codec of the first packet") {
  const auto page = [](std::string_view packet) {
    return "OggS\0\x02"s + std::string(20, '\0') + "\x01"s + static_cast<char>(packet.size()) + std::string(packet);
  };
  CHECK(id_of(Family::Ogg, page("\x01vorbis\0\0\0\0")) == "ogg");
  CHECK(id_of(Family::Ogg, page("OpusHead\x01\x02")) == "opus");
  CHECK(id_of(Family::Ogg, page("\x7F" "FLAC\x01\0")) == "oga");
  CHECK(id_of(Family::Ogg, page("Speex   1.2")) == "spx");
  CHECK(id_of(Family::Ogg, page("\x80theora\x03\x02")) == "ogv");
  CHECK(id_of(Family::Ogg, page("\x80theora\x03\x02") + page("\x01vorbis\0\0\0\0")) == "ogv");
  all_prefixes(page("OpusHead\x01\x02"), "x.opus");
}

TEST_CASE("sniffers: MPEG audio frames, transport and program streams") {
  // MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, no padding: 417-byte frames.
  const std::string frame = "\xFF\xFB\x90\x00"s + std::string(413, '\0');
  CHECK(id_of(Family::Mpeg, frame + frame + frame) == "mp3");
  CHECK(id_of(Family::Mpeg, frame) == "-");  // one frame is not enough without a tag
  const std::string id3 = "ID3\x04\0\0\0\0\0\x0A"s + std::string(10, '\0');
  CHECK(id_of(Family::Mpeg, id3 + frame) == "mp3");
  // Layer II (FF FD), 128 kbit/s (index 8): 144 * 128000 / 44100 = 417 bytes as well.
  const std::string layer2 = "\xFF\xFD\x80\x00"s + std::string(413, '\0');
  CHECK(id_of(Family::Mpeg, layer2 + layer2 + layer2) == "mp2");
  // ADTS: 7-byte header with a 13-bit frame length of 16.
  const std::string adts = "\xFF\xF1\x50\x80\x02\x1F\xFC"s + std::string(9, '\0');
  CHECK(id_of(Family::Mpeg, adts + adts + adts) == "aac");
  const std::string packet = "\x47"s + std::string(187, '\0');
  CHECK(id_of(Family::Mpeg, packet + packet + packet) == "mpegts");
  const std::string bdav = "\0\0\0\0\x47"s + std::string(187, '\0');
  CHECK(id_of(Family::Mpeg, bdav + bdav + bdav) == "m2ts");
  CHECK(id_of(Family::Mpeg, "\0\0\x01\xBA\x44\0\x04\0\x04\x01"s) == "mpg");
  CHECK(id_of(Family::Mpeg, "\0\0\x01\xBA\x44\0\x04\0\x04\x01"s, "movie.vob") == "vob");
  CHECK(id_of(Family::Mpeg, "\0\0\x01\xB3\x04\0\x30\x13"s) == "m1v");
  CHECK(id_of(Family::Mpeg, "\0\0\x01\xB3\x04\0\x30\x13\0\0\x01\xB5\x14"s) == "m2v");
  CHECK(id_of(Family::Mpeg, "\0\0\0\x01\x67\x42\0\x0A"s) == "h264");
  CHECK(id_of(Family::Mpeg, "\0\0\0\x01\x40\x01\x0C\x01"s) == "hevc");
  CHECK(id_of(Family::Mpeg, "\x0B\x77\0\0\0\x40"s) == "ac3");
  CHECK(id_of(Family::Mpeg, "\x0B\x77\0\0\0\x80"s) == "eac3");
  CHECK(id_of(Family::Mpeg, id3 + "fLaC") == "flac");
  all_prefixes(id3 + frame, "x.mp3");
}

TEST_CASE("sniffers: TIFF dialects by IFD tags") {
  const auto tiff = [](std::uint16_t tag, std::string_view ascii = {}) {
    // Little-endian header, IFD at 8 with one entry, ASCII values after the IFD.
    std::string out = "II\x2A\0\x08\0\0\0"s + u16le(1) + u16le(tag);
    if (ascii.empty()) {
      out += u16le(1) + u32le(1) + u32le(1);
    } else {
      out += u16le(2) + u32le(static_cast<std::uint32_t>(ascii.size())) + u32le(26);
    }
    out += "\0\0\0\0"s;
    return out + std::string(ascii);
  };
  CHECK(id_of(Family::Tiff, tiff(0x0100)) == "tiff");
  CHECK(id_of(Family::Tiff, tiff(0x0100), "shot.nrw") == "tiff");  // plain TIFF: only a named row can claim it
  CHECK(id_of(Family::Tiff, tiff(0xC612)) == "dng");
  CHECK(id_of(Family::Tiff, tiff(34735)) == "geotiff");
  CHECK(id_of(Family::Tiff, tiff(0x010F, "NIKON CORPORATION")) == "nef");
  CHECK(id_of(Family::Tiff, tiff(0x010F, "NIKON CORPORATION"), "x.tif") == "tiff");
  CHECK(id_of(Family::Tiff, tiff(0x010E, "<?xml version=\"1.0\"?><OME>")) == "ome.tiff");
  CHECK(id_of(Family::Tiff, "II\x2B\0\x08\0\0\0"s) == "bigtiff");
  CHECK(id_of(Family::Tiff, "IIRO\x08\0\0\0"s) == "orf");
  CHECK(id_of(Family::Tiff, "II\x2A\0\x10\0\0\0CR\x02\0"s) == "cr2");
  all_prefixes(tiff(0x010F, "NIKON CORPORATION"), "x.nef");
}

TEST_CASE("sniffers: tar by magic or header checksum") {
  std::string header(512, '\0');
  header.replace(0, 9, "hello.txt");
  header.replace(100, 8, "0000644\0"s);
  header.replace(124, 12, "00000000000\0"s);
  header[156] = '0';
  // v7 header: checksum of the header with the checksum field as spaces.
  std::string v7 = header;
  v7.replace(148, 8, "        ");
  unsigned sum = 0;
  for (const char c : v7) sum += static_cast<unsigned char>(c);
  char field[8] = {};
  for (int i = 5; i >= 0; --i, sum >>= 3) field[i] = static_cast<char>('0' + (sum & 7));
  v7.replace(148, 8, std::string(field, 6) + "\0 "s);
  CHECK(id_of(Family::Tar, v7) == "tar");
  std::string broken = v7;
  broken[148] = '7';
  CHECK(id_of(Family::Tar, broken) == "-");
  std::string ustar = header;
  ustar.replace(257, 6, "ustar\0"s);
  CHECK(id_of(Family::Tar, ustar) == "tar");
  std::string gem = ustar;
  gem.replace(0, 12, "metadata.gz\0"s);
  CHECK(id_of(Family::Tar, gem) == "gem");
  // gzip-compressed tar: the first decompressed block decides.
  const auto gz = util::gzip_compress(as_bytes(ustar), 6);
  REQUIRE_OK(gz);
  CHECK(id_of(Family::Gzip, as_chars(*gz)) == "tar.gz");
  const auto svgz = util::gzip_compress(as_bytes("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"sv), 6);
  REQUIRE_OK(svgz);
  CHECK(id_of(Family::Gzip, as_chars(*svgz)) == "svgz");
  const auto plain = util::gzip_compress(as_bytes("hello\n"sv), 6);
  REQUIRE_OK(plain);
  CHECK(id_of(Family::Gzip, as_chars(*plain)) == "gz");
  all_prefixes(as_chars(*gz), "x.tgz");
}

TEST_CASE("sniffers: executables and their kinds") {
  // PE: MZ header pointing at "PE\0\0", COFF header with the DLL flag, PE32+ optional header.
  std::string pe = "MZ"s + std::string(0x3A, '\0') + u32le(0x40) + "PE\0\0"s + u16le(0x8664) + u16le(1) + std::string(12, '\0') +
                   u16le(240) + u16le(0x2022) + u16le(0x20B) + std::string(66, '\0') + u16le(2) + std::string(170, '\0');
  CHECK(id_of(Family::Exec, pe) == "dll");
  CHECK(id_of(Family::Exec, pe, "control.cpl") == "cpl");
  std::string exe = pe;
  exe[0x40 + 22] = 0x22;
  exe[0x40 + 23] = 0x00;
  CHECK(id_of(Family::Exec, exe) == "exe");
  CHECK(id_of(Family::Exec, exe, "saver.scr") == "scr");
  exe[0x40 + 24 + 68] = 10;  // EFI application subsystem
  CHECK(id_of(Family::Exec, exe) == "efi");
  // ELF: 64-bit little-endian, e_type at 16.
  const auto elf = [](std::uint16_t type) { return "\x7F" "ELF\x02\x01\x01\0"s + std::string(8, '\0') + u16le(type) + std::string(46, '\0'); };
  CHECK(id_of(Family::Exec, elf(1)) == "o");
  CHECK(id_of(Family::Exec, elf(1), "driver.ko") == "ko");
  CHECK(id_of(Family::Exec, elf(2)) == "elf");
  CHECK(id_of(Family::Exec, elf(4)) == "core");
  CHECK(id_of(Family::Exec, "\x7F" "ELF\x02\x01\x01\0AI\x02"s + std::string(60, '\0')) == "appimage");
  // Mach-O 64-bit little-endian: filetype at 12.
  const auto macho = [](std::uint32_t type) { return "\xCF\xFA\xED\xFE"s + u32le(0x0100000C) + u32le(0) + u32le(type) + std::string(16, '\0'); };
  CHECK(id_of(Family::Exec, macho(2)) == "macho");
  CHECK(id_of(Family::Exec, macho(6)) == "dylib");
  CHECK(id_of(Family::Exec, macho(8)) == "bundle");
  CHECK(id_of(Family::Exec, macho(1)) == "o");
  // CA FE BA BE: class file (major version 52) vs universal binary (two slices).
  CHECK(id_of(Family::Exec, "\xCA\xFE\xBA\xBE\0\0\0\x34"s) == "class");
  const std::string fat = "\xCA\xFE\xBA\xBE"s + u32be(1) + u32be(0x0100000C) + u32be(0) + u32be(28) + u32be(32) + u32be(12) + macho(6);
  CHECK(id_of(Family::Exec, fat) == "dylib");
  CHECK(id_of(Family::Exec, "\xCA\xFE\xBA\xBE\0\0\0\x02"s) == "macho");
  all_prefixes(pe, "x.dll");
  all_prefixes(fat, "x.dylib");
}

TEST_CASE("sniffers: disk images, SQLite, ASF and DER") {
  std::string mbr(512, '\0');
  mbr[446 + 4] = '\x83';
  mbr[510] = '\x55';
  mbr[511] = '\xAA';
  CHECK(id_of(Family::Disk, mbr) == "img");
  std::string bad_mbr = mbr;
  bad_mbr[446] = '\x12';  // an invalid status byte
  CHECK(id_of(Family::Disk, bad_mbr) == "-");
  std::string fat(512, '\0');
  fat.replace(0, 3, "\xEB\x3C\x90"s);
  fat.replace(11, 2, u16le(512));
  fat.replace(54, 8, "FAT12   ");
  CHECK(id_of(Family::Disk, fat) == "img");
  CHECK(id_of(Family::Disk, std::string(1024, '\0') + "conectix" + std::string(504, '\0')) == "vhd");
  CHECK(id_of(Family::Disk, std::string(1024, '\0') + "koly" + std::string(508, '\0')) == "dmg");

  std::string sqlite = "SQLite format 3\0"s + std::string(84, '\0');
  CHECK(id_of(Family::Sqlite, sqlite) == "sqlite");
  sqlite.replace(68, 4, "GPKG");
  CHECK(id_of(Family::Sqlite, sqlite) == "gpkg");
  sqlite.replace(68, 4, "MPBX");
  CHECK(id_of(Family::Sqlite, sqlite) == "mbtiles");

  const std::string asf_header = "\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C"s + u32le(200) + u32le(0) + u32le(1) + "\x01\x02"s;
  const std::string stream = "\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65"s + u32le(100) + u32le(0);
  const std::string audio = "\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B"s;
  const std::string video = "\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B"s;
  CHECK(id_of(Family::Asf, asf_header + stream + audio + std::string(60, '\0')) == "wma");
  CHECK(id_of(Family::Asf, asf_header + stream + video + std::string(60, '\0')) == "wmv");

  // DER: PKCS #7 signedData, PKCS #12 (version 3), certificate (explicit version tag).
  CHECK(id_of(Family::Der, "\x30\x0B\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x07\x02"s) == "p7b");
  CHECK(id_of(Family::Der, "\x30\x0B\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x07\x02"s, "mail.p7s") == "p7s");
  CHECK(id_of(Family::Der, "\x30\x0A\x02\x01\x03\x30\x05\x06\x03\x2A\x03\x04"s) == "p12");
  CHECK(id_of(Family::Der, "\x30\x0C\x30\x08\xA0\x03\x02\x01\x02\x02\x01\x01\x30\x00"s) == "crt");
  CHECK(id_of(Family::Der, "\x30\x0D\x02\x01\x00\x30\x05\x06\x03\x2B\x65\x70\x04\x02\x04\x00"s) == "private-key");
  all_prefixes(fat, "x.img");
}

TEST_CASE("sniffers: XML dialects and HTML") {
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?>\n<!-- c --><svg xmlns=\"http://www.w3.org/2000/svg\"/>") == "svg");
  CHECK(id_of(Family::Markup, "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document/></kml>") == "kml");
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?><feed xmlns=\"http://www.w3.org/2005/Atom\"/>") == "atom");
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?><feed/>") == "xml");  // no Atom namespace
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?><rdf:RDF xmlns:rdf=\"r\" xmlns=\"http://purl.org/rss/1.0/\"/>") == "rss");
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?><MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\"/>") == "mpd");
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?><Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"/>",
              "app.vcxproj") == "vcxproj");
  CHECK(id_of(Family::Markup, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"x\">\n<plist version=\"1.0\"/>") == "plist");
  CHECK(id_of(Family::Markup, "<!DOCTYPE html>\n<html><body/></html>") == "html");
  CHECK(id_of(Family::Markup, "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"/>") == "xhtml");
  CHECK(id_of(Family::Markup, "bplist00\xD0\x08\0\0\0\0\0\0\x01\x01"s) == "plist");
  CHECK(id_of(Family::Markup, "\xFF\xFE<\0?\0x\0m\0l\0 \0?\0>\0<\0s\0v\0g\0/\0>\0"s) == "svg");  // UTF-16 with BOM
  CHECK(id_of(Family::Markup, "plain words") == "-");
}

TEST_CASE("sniffers: JSON dialects") {
  CHECK(id_of(Family::Json, "{\"type\": \"FeatureCollection\", \"features\": []}") == "geojson");
  CHECK(id_of(Family::Json, "{\"type\": \"Topology\", \"objects\": {}, \"arcs\": []}") == "topojson");
  CHECK(id_of(Family::Json, "{\"asset\": {\"version\": \"2.0\"}, \"nodes\": []}") == "gltf");
  CHECK(id_of(Family::Json, "{\"cells\": [], \"metadata\": {}, \"nbformat\": 4}") == "ipynb");
  CHECK(id_of(Family::Json, "{\"log\": {\"version\": \"1.2\", \"entries\": []}}") == "har");
  CHECK(id_of(Family::Json, "{\"@context\": \"https://schema.org\"}") == "jsonld");
  CHECK(id_of(Family::Json, "{\"a\": 1}\n{\"a\": 2}\n") == "jsonl");
  CHECK(id_of(Family::Json, "{\"a\": [1, {\"b\": \"}\"}]}") == "json");
  CHECK(id_of(Family::Json, "{\"a\": 1} trailing") == "-");
  CHECK(id_of(Family::Json, "{'not': json}") == "-");
}

TEST_CASE("sniffers: line-oriented text formats") {
  CHECK(id_of(Family::Text, "#!/usr/bin/env -S python3 -u\nprint(1)\n", "", "py") == "py");
  CHECK(id_of(Family::Text, "#!/bin/bash\necho\n", "", "sh") == "sh");
  CHECK(id_of(Family::Text, "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n", "", "crt") == "crt");
  CHECK(id_of(Family::Text, "-----BEGIN CERTIFICATE-----\nMIIB\n", "chain.pem", "pem") == "pem");
  CHECK(id_of(Family::Text, "1\n00:00:01,000 --> 00:00:02,000\nHi\n", "", "srt") == "srt");
  CHECK(id_of(Family::Text, "#EXTM3U\n#EXT-X-VERSION:3\n", "", "m3u8") == "m3u8");
  CHECK(id_of(Family::Text, "#EXTM3U\na.mp3\n", "", "m3u") == "m3u");
  CHECK(id_of(Family::Text, "a,b\n1,2\n3,4\n", "", "csv") == "csv");
  CHECK(id_of(Family::Text, "a\tb\n1\t2\n", "", "tsv") == "tsv");
  CHECK(id_of(Family::Text, "CREATE TABLE t (a INTEGER);\nINSERT INTO t VALUES (1);\nSELECT a FROM t;\n", "", "csv") == "-");
  CHECK(id_of(Family::Text, "From a@b Thu Jan  1 00:00:00 1970\nFrom: a@b\n\nHi\n", "", "mbox") == "mbox");
  CHECK(id_of(Family::Text, "@interface A\n@end\n", "a.m", "m") == "m");
  CHECK(id_of(Family::Text, "function y = f(x)\n  y = x;\nend\n", "f.m", "matlab") == "matlab");
  CHECK(id_of(Family::Text, "all:\n\ttrue\n", "Makefile", "makefile") == "makefile");
  CHECK(id_of(Family::Text, "Just words, nothing else.\n", "", "csv") == "-");
  CHECK(id_of(Family::Text, "\x01\x02\x03\x04\x05\x06\x07\x08\x0E\x0F"s, "", "srt") == "-");
}

TEST_CASE("sniffers: format-specific structural checks") {
  CHECK(id_of(Family::Misc, "P6\n# c\n4 4\n255\n", "", "ppm") == "ppm");
  CHECK(id_of(Family::Misc, "P5 4 4 255\n", "", "ppm") == "pgm");
  CHECK(id_of(Family::Misc, "P6x", "", "ppm") == "-");
  const std::string png = "\x89PNG\r\n\x1A\n"s;
  const std::string ihdr = u32be(13) + "IHDR" + std::string(13, '\0') + "CRC!";
  CHECK(id_of(Family::Misc, png + ihdr + u32be(8) + "acTL" + std::string(8, '\0') + "CRC!", "", "apng") == "apng");
  CHECK(id_of(Family::Misc, png + ihdr + u32be(0) + "IDAT" + "CRC!", "", "apng") == "-");
  CHECK(id_of(Family::Misc, "\xCB\x0D\x0D\x0A\0\0\0\0"s, "", "pyc") == "pyc");  // Python 3.12
  CHECK(id_of(Family::Misc, "\x80\x04\x95\x05\0\0\0\0\0\0\0N."s, "", "pkl") == "pkl");
  const std::string header = "{\"x\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}";
  CHECK(id_of(Family::Misc, u32le(static_cast<std::uint32_t>(header.size())) + "\0\0\0\0"s + header + "\0\0\0\0"s, "", "safetensors") ==
        "safetensors");
  CHECK(id_of(Family::Misc, "d8:announce3:url4:infod4:name1:xee", "", "torrent") == "torrent");
  std::string stl(84 + 50, '\0');
  stl[80] = 1;
  CHECK(id_of(Family::Misc, stl, "", "stl") == "stl");
  CHECK(id_of(Family::Misc, stl + "x", "", "stl") == "-");  // the triangle count must match the size
  const std::string ico_entry = "\x10\x10\0\0\x01\0\x20\0"s + u32le(100) + u32le(22);
  CHECK(id_of(Family::Misc, "\0\0\x01\0\x01\0"s + ico_entry, "", "ico") == "ico");
  CHECK(id_of(Family::Misc, "\0\0\x02\0\x01\0"s + ico_entry, "", "ico") == "cur");
  std::string dbf(65, '\0');
  dbf[0] = 0x03;
  dbf[1] = 124;
  dbf[2] = 1;
  dbf[3] = 1;
  dbf.replace(8, 2, u16le(65));
  dbf.replace(10, 2, u16le(11));
  dbf[64] = 0x0D;
  CHECK(id_of(Family::Misc, dbf, "", "dbf") == "dbf");
  std::string gb(0x150, '\0');
  gb.replace(0x104, 8, "\xCE\xED\x66\x66\xCC\x0D\x00\x0B"s);
  CHECK(id_of(Family::Misc, gb, "", "gb") == "gb");
  gb[0x143] = '\x80';
  CHECK(id_of(Family::Misc, gb, "", "gb") == "gbc");
  all_prefixes(stl, "x.stl");
  all_prefixes(dbf, "x.dbf");
}

TEST_CASE("sniffers: large and adversarial inputs stay bounded") {
  // Repeated container magic, deep nesting and a huge claimed directory.
  std::string pk;
  for (int i = 0; i < 20000; ++i) pk += "PK\x03\x04"s;
  (void)sniff::classify(Family::Zip, as_bytes(pk), "x.zip", "docx");
  std::string eocd = "PK\x03\x04"s + std::string(100, '\0') + "PK\x05\x06\0\0\0\0\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\0\0\0\0\0\0"s;
  CHECK(id_of(Family::Zip, eocd, "", "docx") == "-");
  std::string json(60000, '[');
  CHECK(id_of(Family::Json, json) != "geojson");
  std::string ogg;
  for (int i = 0; i < 100; ++i) ogg += "OggS\0\x02"s + std::string(20, '\0') + "\x01\xFF"s + std::string(255, 'x');
  (void)sniff::classify(Family::Ogg, as_bytes(ogg), {}, "ogg");
  const Bytes noise = test::random_bytes(70000, 7);
  for (int f = 0; f <= static_cast<int>(Family::Misc); ++f) (void)sniff::classify(static_cast<Family>(f), noise, "x.bin", "ico");
}
