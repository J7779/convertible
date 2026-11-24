// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// E-mail: RFC 5322 / MIME messages (.eml), mbox mailboxes and Apple .emlx.
#include "engine/detect.hpp"
#include "ir/archive.hpp"
#include "ir/document.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"

using namespace cvt;

namespace {

std::string detected(std::string_view fixture_name, std::string_view filename) {
  const Bytes data = test::fixture(fixture_name);
  const auto detection = detect_format(Registry::instance(), data, filename);
  return detection ? detection->format->id : std::string("<nothing>");
}

Bytes attachment_bytes() {
  Bytes out;
  for (int repeat = 0; repeat < 4; ++repeat) {
    for (int b = 0; b < 256; ++b) out.push_back(static_cast<std::byte>(b));
  }
  return out;
}

bool html_reader_registered() {
  for (const Codec* codec : Registry::instance().codecs_for("html")) {
    if (codec->kind == Kind::Document && codec->read) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("pim/eml: a multipart message written by Python's email package") {
  const auto document = test::read_as<ir::Document>("eml", test::fixture("pim/mail/multipart.eml"));
  REQUIRE_OK(document);
  CHECK(document->metadata.title == "Meeting notes 📝 — שלום");
  CHECK(document->metadata.author == "Ada Example <ada@example.org>");
  CHECK(document->metadata.date == "2024-05-01T12:30:00+02:00");
  const std::string text = ir::plain_text(*document);
  // Encoded words in To, quoted-printable body with soft breaks and UTF-8.
  CHECK(text.find("Ben Sample <ben@example.org>, Chloé Exemple <chloe@example.org>") != std::string::npos);
  CHECK(text.find("Here are the notes: café, naïve, 😀.") != std::string::npos);
  CHECK(text.find("A very long line " + std::string(120, 'x')) != std::string::npos);
  CHECK(text.find("données été.bin (application/octet-stream, 1.0 KB)") != std::string::npos);

  const auto archive = test::read_as<ir::Archive>("eml", test::fixture("pim/mail/multipart.eml"));
  REQUIRE_OK(archive);
  REQUIRE(archive->entries.size() == 1);
  CHECK(archive->entries[0].path == "données été.bin");
  CHECK(archive->entries[0].bytes == attachment_bytes());
}

TEST_CASE("pim/eml: HTML-only Latin-1 message and a forwarded message") {
  std::vector<std::string> warnings;
  const auto document = test::read_as<ir::Document>("eml", test::fixture("pim/mail/html_latin1.eml"), {}, &warnings);
  REQUIRE_OK(document);
  CHECK(document->metadata.title == "Prix spécial");
  CHECK(ir::plain_text(*document).find("Café crème & gâteau") != std::string::npos);
  if (!html_reader_registered()) CHECK(!warnings.empty());  // degraded to plain text

  const auto forward = test::read_as<ir::Archive>("eml", test::fixture("pim/mail/forward.eml"));
  REQUIRE_OK(forward);
  REQUIRE(forward->entries.size() == 2);
  CHECK(forward->entries[0].path == "attachment-1.eml");
  CHECK(test::str(forward->entries[0].bytes).find("Subject: Original") != std::string::npos);
  CHECK(forward->entries[1].path == "report.pdf");
  CHECK(test::str(forward->entries[1].bytes) == "%PDF-1.4 synthetic\n");
  const auto forward_doc = test::read_as<ir::Document>("eml", test::fixture("pim/mail/forward.eml"));
  REQUIRE_OK(forward_doc);
  CHECK(ir::plain_text(*forward_doc).find("See the forwarded message.") != std::string::npos);
}

TEST_CASE("pim/mbox and emlx: mailboxes and Apple Mail files") {
  const auto archive = test::read_as<ir::Archive>("mbox", test::fixture("pim/mail/mailbox.mbox"));
  REQUIRE_OK(archive);
  REQUIRE(archive->entries.size() == 3);
  CHECK(archive->entries[0].path == "0001 First.eml");
  CHECK(archive->entries[1].path == "0002 Second ünïcödé.eml");
  // The mboxo-escaped body line is restored.
  CHECK(test::str(archive->entries[1].bytes).find("\nFrom the start of a line\n") != std::string::npos);
  // Each stored message reads as a message again.
  const auto second = test::read_as<ir::Document>("eml", archive->entries[1].bytes);
  REQUIRE_OK(second);
  CHECK(second->metadata.title == "Second ünïcödé");
  const auto all = test::read_as<ir::Document>("mbox", test::fixture("pim/mail/mailbox.mbox"));
  REQUIRE_OK(all);
  const std::string text = ir::plain_text(*all);
  CHECK(text.find("Plain body one.") != std::string::npos);
  CHECK(text.find("Body three.") != std::string::npos);

  const auto emlx = test::read_as<ir::Document>("emlx", test::fixture("pim/mail/message.emlx"));
  REQUIRE_OK(emlx);
  CHECK(emlx->metadata.title == "Meeting notes 📝 — שלום");
  const auto emlx_files = test::read_as<ir::Archive>("emlx", test::fixture("pim/mail/message.emlx"));
  REQUIRE_OK(emlx_files);
  REQUIRE(emlx_files->entries.size() == 1);
  CHECK(emlx_files->entries[0].bytes == attachment_bytes());
}

TEST_CASE("pim/mail: hostile names, encodings and structures") {
  // Attachment names cannot escape the archive root; duplicates get numbered.
  const std::string_view message =
      "From: a@example.org\r\nSubject: =?UTF-8?Q?caf=C3=A9_=E2=98=95?= =?UTF-8?B?IOKclA==?=\r\n"
      "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=XYZ\r\n\r\n"
      "--XYZ\r\nContent-Type: text/plain\r\n\r\nbody\r\n"
      "--XYZ\r\nContent-Type: application/octet-stream; name=\"../../etc/passwd\"\r\n"
      "Content-Transfer-Encoding: base64\r\n\r\naGVsbG8=\r\n"
      "--XYZ\r\nContent-Disposition: attachment; filename*=UTF-8''pass%C3%A9.txt\r\n\r\none\r\n"
      "--XYZ\r\nContent-Disposition: attachment; filename*=UTF-8''pass%C3%A9.txt\r\n\r\ntwo\r\n"
      "--XYZ--\r\n";
  const auto archive = test::read_as<ir::Archive>("eml", test::bytes(message));
  REQUIRE_OK(archive);
  REQUIRE(archive->entries.size() == 3);
  CHECK(archive->entries[0].path == "passwd");
  CHECK(test::str(archive->entries[0].bytes) == "hello");
  CHECK(archive->entries[1].path == "passé.txt");
  CHECK(archive->entries[2].path == "passé (2).txt");
  const auto document = test::read_as<ir::Document>("eml", test::bytes(message));
  REQUIRE_OK(document);
  CHECK(document->metadata.title == "café ☕ ✔");  // adjacent encoded words join without the space
  // Unknown charsets degrade with a warning.
  std::vector<std::string> warnings;
  const auto odd = test::read_as<ir::Document>(
      "eml", test::bytes("From: a@example.org\nSubject: x\nContent-Type: text/plain; charset=koi8-u\n\nabc\n"), {}, &warnings);
  REQUIRE_OK(odd);
  CHECK(!warnings.empty());
  // Deep nesting stops at a limit instead of recursing without bound.
  std::string nested = "From: a@example.org\nSubject: deep\n";
  for (int i = 0; i < 100; ++i) nested += std::format("Content-Type: multipart/mixed; boundary=b{}\n\n--b{}\n", i, i);
  nested += "\nleaf\n";
  CHECK_STATUS(test::read_as<ir::Document>("eml", test::bytes(nested)), Status::Limit);
  CHECK_STATUS(test::read_as<ir::Document>("eml", test::bytes("just some text\nwithout headers\n")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Archive>("emlx", test::bytes("99999\nFrom: a\n")), Status::Decode);
}

TEST_CASE("pim/mail: detection and damaged input") {
  CHECK(detected("pim/mail/multipart.eml", "") == "eml");
  CHECK(detected("pim/mail/html_latin1.eml", "x.eml") == "eml");
  CHECK(detected("pim/mail/mailbox.mbox", "") == "mbox");
  CHECK(detected("pim/mail/message.emlx", "") == "emlx");
  for (const char* format : {"eml", "mbox", "emlx"}) {
    INFO(format);
    const char* file = std::string_view(format) == "eml" ? "pim/mail/multipart.eml"
                       : std::string_view(format) == "mbox" ? "pim/mail/mailbox.mbox"
                                                            : "pim/mail/message.emlx";
    CHECK(test::fuzz_reader<ir::Document>(format, test::fixture(file)) > 0);
    CHECK(test::fuzz_reader<ir::Archive>(format, test::fixture(file), 128) > 0);
  }
  CHECK(test::fuzz_reader<ir::Archive>("eml", test::fixture("pim/mail/forward.eml")) > 0);
}
