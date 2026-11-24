// Convertible core - office family: comic book archives.
//
// CBZ (ZIP) and CBT (tar) are read in-house; CBR (RAR) and CB7 (7-Zip) go
// through the archive family's readers at run time and fail with a clear
// message when those are not available. Pages are the pictures of the
// archive in natural name order ("page2" before "page10"); ComicInfo.xml
// gives the title. Comics read as a Document (one picture per page, page
// breaks between - so comics convert to PDF and EPUB) or as an Image of
// frames (pages fitted onto the first page's canvas). CBZ is also written,
// from a Document's pictures or an Image's frames.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/document/document_api.hpp"
#include "families/image/image_api.hpp"
#include "families/image/image_ops.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "ir/archive.hpp"
#include "util/text.hpp"
#include "util/zip.hpp"

namespace cvt::office {

namespace {

struct Page {
  std::string name;
  Bytes bytes;
};

bool picture_name(std::string_view name) {
  const std::string lower = util::to_lower(name);
  if (lower.starts_with("__macosx/") || lower.find("/.") != std::string::npos || lower.starts_with(".")) return false;
  for (std::string_view ext : {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".avif", ".jxl"}) {
    if (lower.ends_with(ext)) return true;
  }
  return false;
}

// "page2" < "page10": digits compare as numbers.
bool natural_less(std::string_view a, std::string_view b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (util::is_ascii_digit(a[i]) && util::is_ascii_digit(b[j])) {
      std::size_t ie = i;
      std::size_t je = j;
      while (ie < a.size() && util::is_ascii_digit(a[ie])) ++ie;
      while (je < b.size() && util::is_ascii_digit(b[je])) ++je;
      std::string_view na = a.substr(i, ie - i);
      std::string_view nb = b.substr(j, je - j);
      while (na.size() > 1 && na.front() == '0') na.remove_prefix(1);
      while (nb.size() > 1 && nb.front() == '0') nb.remove_prefix(1);
      if (na.size() != nb.size()) return na.size() < nb.size();
      if (na != nb) return na < nb;
      i = ie;
      j = je;
      continue;
    }
    const char ca = util::ascii_lower(a[i]);
    const char cb = util::ascii_lower(b[j]);
    if (ca != cb) return ca < cb;
    ++i;
    ++j;
  }
  return a.size() - i < b.size() - j;
}

// Minimal ustar/GNU tar member list (regular files only).
Result<std::vector<Page>> tar_pages(ByteView data, const Limits& limits, std::string* comic_info) {
  std::vector<Page> pages;
  std::size_t at = 0;
  std::uint64_t total = 0;
  std::size_t count = 0;
  std::string long_name;
  while (at + 512 <= data.size()) {
    const std::string_view header = as_chars(data.subspan(at, 512));
    if (header.find_first_not_of('\0') == std::string_view::npos) break;
    // Checksum check: the header must be a tar header.
    unsigned sum = 0;
    for (std::size_t k = 0; k < 512; ++k) sum += (k >= 148 && k < 156) ? ' ' : static_cast<unsigned char>(header[k]);
    const auto stored = util::parse_int(util::trim(header.substr(148, 8).substr(0, header.substr(148, 8).find('\0'))), 8);
    if (!stored || static_cast<unsigned>(*stored) != sum) return fail(Status::Decode, "corrupt tar header");
    std::string name(header.substr(0, header.substr(0, 100).find('\0')));
    if (header.substr(257, 5) == "ustar") {
      const std::string prefix(header.substr(345, header.substr(345, 155).find('\0')));
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (!long_name.empty()) {
      name = long_name;
      long_name.clear();
    }
    const std::string_view size_field = util::trim(header.substr(124, 12).substr(0, header.substr(124, 12).find('\0')));
    const auto size = util::parse_int(size_field.empty() ? std::string_view("0") : size_field, 8);
    if (!size || *size < 0) return fail(Status::Decode, "corrupt tar size");
    const char type = header[156];
    const std::uint64_t length = static_cast<std::uint64_t>(*size);
    at += 512;
    if (at + length > data.size()) return fail(Status::Decode, "the tar archive is truncated");
    CVT_TRY(limits.check_archive_entries(++count));
    if (type == 'L') {
      long_name = std::string(as_chars(data.subspan(at, static_cast<std::size_t>(length))));
      long_name = long_name.substr(0, long_name.find('\0'));
    } else if (type == '0' || type == '\0') {
      if (util::iends_with(name, "comicinfo.xml") && comic_info) {
        *comic_info = std::string(as_chars(data.subspan(at, static_cast<std::size_t>(length))));
      } else if (picture_name(name)) {
        total += length;
        CVT_TRY(limits.check_decompressed(total));
        pages.push_back(Page{name, to_bytes(data.subspan(at, static_cast<std::size_t>(length)))});
      }
    }
    at += static_cast<std::size_t>((length + 511) / 512 * 512);
  }
  return pages;
}

Result<std::vector<Page>> comic_pages(const ReadContext& ctx, std::string* comic_info) {
  const std::string_view head = as_chars(ctx.input).substr(0, 8);
  std::vector<Page> pages;
  if (head.starts_with("PK")) {
    CVT_TRY_ASSIGN(util::ZipReader zip, util::ZipReader::open(ctx.input, ctx.limits));
    for (std::size_t i = 0; i < zip.size(); ++i) {
      const util::ZipEntry& e = zip.entries()[i];
      if (e.is_directory) continue;
      if (util::iends_with(e.name, "comicinfo.xml") && comic_info) {
        if (auto b = zip.read(i)) *comic_info = std::string(as_chars(*b));
        continue;
      }
      if (!picture_name(e.name)) continue;
      if (e.encrypted) return fail(Status::Decode, "the comic archive is password-protected");
      CVT_TRY_ASSIGN(Bytes bytes, zip.read(i));
      pages.push_back(Page{e.name, std::move(bytes)});
    }
  } else if (as_chars(ctx.input).size() > 262 && as_chars(ctx.input).substr(257, 5) == "ustar") {
    CVT_TRY_ASSIGN(pages, tar_pages(ctx.input, ctx.limits, comic_info));
  } else {
    // RAR and 7-Zip: the archive family's readers, when present.
    const bool rar = head.starts_with("Rar!");
    const bool seven = head.starts_with("7z\xBC\xAF\x27\x1C");
    if (!rar && !seven) {
      // Some CBT files are old (pre-ustar) tar archives.
      auto tar = tar_pages(ctx.input, ctx.limits, comic_info);
      if (tar) {
        pages = std::move(*tar);
      } else {
        return fail(Status::Decode, "unknown comic archive type");
      }
    } else {
      const Registry& registry = ctx.registry ? *ctx.registry : Registry::instance();
      const std::string format = rar ? "rar" : "7z";
      const Codec* reader = nullptr;
      for (const Codec* codec : registry.codecs_for(format)) {
        if (codec->kind == Kind::Archive && codec->read) reader = codec;
      }
      if (!reader) {
        return fail(Status::UnsupportedConversion, "{} comic archives need the {} reader, which is not available in this build",
                    rar ? "CBR" : "CB7", rar ? "RAR" : "7-Zip");
      }
      ReadContext inner;
      inner.limits = ctx.limits;
      inner.progress = ctx.progress;
      inner.warnings = ctx.warnings;
      inner.registry = &registry;
      inner.input = ctx.input;
      inner.format_id = format;
      CVT_TRY_ASSIGN(AnyIR value, reader->read(inner));
      ir::Archive archive = std::move(value).get<ir::Archive>();
      for (ir::ArchiveEntry& e : archive.entries) {
        if (e.is_directory) continue;
        if (util::iends_with(e.path, "comicinfo.xml") && comic_info) {
          *comic_info = std::string(as_chars(e.bytes));
          continue;
        }
        if (picture_name(e.path)) pages.push_back(Page{e.path, std::move(e.bytes)});
      }
    }
  }
  std::ranges::stable_sort(pages, [](const Page& a, const Page& b) { return natural_less(a.name, b.name); });
  if (pages.empty()) return fail(Status::Decode, "the comic archive contains no pictures");
  return pages;
}

std::string comic_title(const std::string& info) {
  if (info.empty()) return {};
  auto xml = parse_untrusted_xml(as_bytes(info), false);
  if (!xml) return {};
  return std::string(util::trim(child(xml->root(), "Title").text()));
}

Result<AnyIR> read_comic_document(ReadContext& ctx) {
  std::string info;
  CVT_TRY_ASSIGN(std::vector<Page> pages, comic_pages(ctx, &info));
  ir::Document doc;
  doc.metadata.title = comic_title(info);
  for (std::size_t i = 0; i < pages.size(); ++i) {
    if (i > 0) doc.blocks.push_back(ir::PageBreak{});
    ir::EncodedImage image;
    image.mime = document::sniff_image_mime(pages[i].bytes);
    if (image.mime.empty()) image.mime = mime_for_extension(pages[i].name);
    image.bytes = std::move(pages[i].bytes);
    doc.blocks.push_back(ir::ImageBlock{std::move(image), "Page " + std::to_string(i + 1), {}});
  }
  return AnyIR(std::move(doc));
}

// Fit a frame onto a canvas of the given size (white background, centred).
Result<ir::Frame> fit(const ir::Frame& frame, std::uint32_t width, std::uint32_t height) {
  if (frame.width == width && frame.height == height) return frame;
  const double scale = std::min(static_cast<double>(width) / frame.width, static_cast<double>(height) / frame.height);
  const auto w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(frame.width * scale)));
  const auto h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(frame.height * scale)));
  CVT_TRY_ASSIGN(const ir::Frame scaled, image::resize(frame, w, h));
  ir::Frame canvas = ir::Frame::filled(width, height, {255, 255, 255, 255});
  const std::uint32_t ox = (width - w) / 2;
  const std::uint32_t oy = (height - h) / 2;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t s = scaled.offset(x, y);
      const std::size_t d = canvas.offset(ox + x, oy + y);
      for (int c = 0; c < 4; ++c) canvas.rgba[d + c] = scaled.rgba[s + c];
    }
  }
  return canvas;
}

Result<AnyIR> read_comic_image(ReadContext& ctx) {
  CVT_TRY_ASSIGN(std::vector<Page> pages, comic_pages(ctx, nullptr));
  ir::Image out;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(pages.size()), "Decoding pages"));
    const auto format = image::sniff_format(pages[i].bytes);
    if (!format) {
      ctx.warn("A page in an unknown picture format was skipped: " + pages[i].name);
      continue;
    }
    CVT_TRY_ASSIGN(ir::Image page, image::decode(*format, pages[i].bytes, ctx));
    if (!page.valid()) continue;
    if (out.frames.empty()) {
      CVT_TRY(ctx.limits.check_pixels(page.width(), page.height(), pages.size()));
      out.frames.push_back(std::move(page.frames.front()));
      continue;
    }
    CVT_TRY_ASSIGN(ir::Frame frame, fit(page.frames.front(), out.frames.front().width, out.frames.front().height));
    out.frames.push_back(std::move(frame));
  }
  if (out.frames.empty()) return fail(Status::Decode, "no page of the comic could be decoded");
  for (ir::Frame& f : out.frames) f.delay_ms = 2000;
  return AnyIR(std::move(out));
}

Result<Bytes> finish_cbz(std::vector<std::pair<std::string, Bytes>>& files, const WriteContext& ctx) {
  if (files.empty()) return fail(Status::Encode, "there are no pictures to put into the comic archive");
  util::ZipWriter zip;
  util::ZipEntryOptions stored;
  stored.level = 0;  // pictures are already compressed
  (void)ctx;
  for (auto& [name, bytes] : files) CVT_TRY(zip.add(name, bytes, stored));
  return zip.finish();
}

std::string page_name(std::size_t index, std::string_view ext) {
  std::string number = std::to_string(index);
  while (number.size() < 3) number.insert(number.begin(), '0');
  return "page-" + number + "." + std::string(ext);
}

Result<Bytes> write_cbz_from_image(const AnyIR& value, WriteContext& ctx) {
  const ir::Image& image = value.get<ir::Image>();
  if (!image.valid()) return fail(Status::Encode, "the image has no pixels");
  const std::string format = ctx.options.get_enum("page_format", "jpg", {"jpg", "png"});
  std::vector<std::pair<std::string, Bytes>> files;
  for (std::size_t i = 0; i < image.frames.size(); ++i) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(image.frames.size()), "Encoding pages"));
    ir::Image page;
    page.frames.push_back(image.frames[i]);
    page.frames.back().delay_ms = 0;
    Options options;
    if (format == "jpg") options.set("quality", Json(static_cast<std::int64_t>(options::resolved_quality(ctx.options))));
    CVT_TRY_ASSIGN(Bytes bytes, image::encode(format, page, options, ctx));
    files.emplace_back(page_name(i + 1, format), std::move(bytes));
  }
  return finish_cbz(files, ctx);
}

Result<Bytes> write_cbz_from_document(const AnyIR& value, WriteContext& ctx) {
  const ir::Document& doc = value.get<ir::Document>();
  ImagePolicy policy = image_policy(ctx.options);
  if (policy.mode == ImageMode::Drop) policy.mode = ImageMode::Keep;
  std::vector<std::pair<std::string, Bytes>> files;
  std::function<void(const ir::Blocks&, int)> walk;
  std::function<void(const ir::Inlines&, int)> walk_inlines = [&](const ir::Inlines& inlines, int depth) {
    if (depth > ir::max_document_depth) return;
    for (const ir::Inline& item : inlines) {
      if (const auto* image = item.get_if<ir::InlineImage>()) {
        if (auto p = prepare_image(image->source, policy, ctx)) files.emplace_back(page_name(files.size() + 1, p->extension), std::move(p->bytes));
      }
      std::visit(
          [&](const auto& node) {
            if constexpr (requires { node.children; }) walk_inlines(node.children, depth + 1);
          },
          item.node);
    }
  };
  walk = [&](const ir::Blocks& blocks, int depth) {
    if (depth > ir::max_document_depth) return;
    for (const ir::Block& b : blocks) {
      if (const auto* image = b.get_if<ir::ImageBlock>()) {
        if (auto p = prepare_image(image->source, policy, ctx)) files.emplace_back(page_name(files.size() + 1, p->extension), std::move(p->bytes));
      }
      if (const auto* p = b.get_if<ir::Paragraph>()) walk_inlines(p->content, 0);
      if (const auto* l = b.get_if<ir::List>()) {
        for (const ir::Blocks& item : l->items) walk(item, depth + 1);
      }
      if (const auto* q = b.get_if<ir::BlockQuote>()) walk(q->blocks, depth + 1);
    }
  };
  walk(doc.blocks, 0);
  if (!doc.metadata.title.empty()) {
    const std::string info = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ComicInfo><Title>" + esc(doc.metadata.title) +
                             "</Title><PageCount>" + std::to_string(files.size()) + "</PageCount></ComicInfo>\n";
    files.emplace_back("ComicInfo.xml", to_bytes(info));
  }
  return finish_cbz(files, ctx);
}

int sniff_cbz(ByteView data, std::string_view filename) {
  const bool ext = util::iends_with(filename, ".cbz");
  if (!as_chars(data).starts_with("PK\x03\x04")) return 0;
  if (zip_mentions(data, "ComicInfo.xml")) return 92;
  return ext ? 85 : 0;
}

}  // namespace

void register_comics(Registry& registry) {
  struct Kind_ {
    const char* id;
    const char* name;
    const char* mime;
    int popularity;
    std::function<int(ByteView, std::string_view)> sniff;
  };
  const std::vector<Kind_> kinds = {
      {"cbz", "Comic book (ZIP)", "application/vnd.comicbook+zip", 35, sniff_cbz},
      {"cbt", "Comic book (tar)", "application/x-cbt", 8,
       [](ByteView data, std::string_view filename) {
         return util::iends_with(filename, ".cbt") && as_chars(data).size() > 262 && as_chars(data).substr(257, 5) == "ustar" ? 90 : 0;
       }},
      {"cbr", "Comic book (RAR)", "application/vnd.comicbook-rar", 30,
       [](ByteView data, std::string_view filename) {
         return util::iends_with(filename, ".cbr") && as_chars(data).starts_with("Rar!") ? 90 : 0;
       }},
      {"cb7", "Comic book (7-Zip)", "application/x-cb7", 8,
       [](ByteView data, std::string_view filename) {
         return util::iends_with(filename, ".cb7") && as_chars(data).starts_with("7z\xBC\xAF\x27\x1C") ? 90 : 0;
       }},
  };
  for (const Kind_& k : kinds) {
    FormatInfo info;
    info.id = k.id;
    info.name = k.name;
    info.description = "Pages of a comic as pictures in an archive.";
    info.category = Category::Ebook;
    info.extensions = {k.id};
    info.mime = k.mime;
    info.popularity = k.popularity;
    info.sniff = k.sniff;
    registry.add_format(std::move(info));
    Codec doc_reader;
    doc_reader.format_id = k.id;
    doc_reader.kind = Kind::Document;
    doc_reader.read = read_comic_document;
    registry.add_codec(std::move(doc_reader));
    Codec image_reader;
    image_reader.format_id = k.id;
    image_reader.kind = Kind::Image;
    image_reader.read = read_comic_image;
    image_reader.read_cost = 30;
    registry.add_codec(std::move(image_reader));
  }
  Codec from_image;
  from_image.format_id = "cbz";
  from_image.kind = Kind::Image;
  from_image.write = write_cbz_from_image;
  from_image.write_options = {OptionSpec::choice("page_format", "Page format", "jpg", {"jpg", "png"},
                                                 "Picture format of the pages."),
                              options::quality(), options::preset()};
  registry.add_codec(std::move(from_image));
  Codec from_document;
  from_document.format_id = "cbz";
  from_document.kind = Kind::Document;
  from_document.write = write_cbz_from_document;
  from_document.write_options = image_write_options();
  from_document.write_cost = 20;
  from_document.lossy = true;
  registry.add_codec(std::move(from_document));
}

}  // namespace cvt::office
