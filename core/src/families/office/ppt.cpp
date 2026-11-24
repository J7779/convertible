// Convertible core - office family: PowerPoint 97-2003 binary presentations.
//
// Per [MS-PPT]: the "Current User" stream leads to the last UserEditAtom,
// whose persist directory maps persist ids to record offsets. Slides are
// taken in the order of the slide list; each slide's drawing is walked for
// its text (text atoms, or references into the slide list's outline text),
// paragraph indent levels and bullets come from the style atoms, pictures
// from the "Pictures" stream through the BLIP store, speaker notes (option)
// from the notes slides.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/registry.hpp"
#include "families/office/escher.hpp"
#include "families/office/office.hpp"
#include "families/office/office_common.hpp"
#include "families/office/slides.hpp"
#include "families/table/cfb.hpp"
#include "families/table/codepages.hpp"
#include "util/text.hpp"

namespace cvt::office {

namespace {

std::uint32_t le32(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u32le();
}
std::uint16_t le16(ByteView d, std::size_t at) {
  util::ByteReader r(d);
  if (!r.seek(at)) return 0;
  return r.u16le();
}

struct Record {
  std::uint16_t ver = 0;
  std::uint16_t inst = 0;
  std::uint16_t type = 0;
  std::uint32_t length = 0;
  std::size_t body = 0;  // offset of the record body
};

std::optional<Record> record_at(ByteView data, std::size_t at) {
  if (at + 8 > data.size()) return std::nullopt;
  Record r;
  const std::uint16_t vi = le16(data, at);
  r.ver = vi & 0x0F;
  r.inst = vi >> 4;
  r.type = le16(data, at + 2);
  r.length = le32(data, at + 4);
  r.body = at + 8;
  if (std::uint64_t{r.body} + r.length > data.size()) return std::nullopt;
  return r;
}

struct TextBlock {
  int type = 4;  // TextHeaderAtom.textType
  std::string text;
  std::vector<int> levels;   // indent level per paragraph
  std::vector<int> bullets;  // -1 unknown, 0 no, 1 yes
};

// Paragraph runs of a StyleTextPropAtom: indent level and bullet flag.
void parse_style(ByteView data, const Record& rec, std::size_t text_length, TextBlock& block) {
  util::ByteReader r(data.subspan(rec.body, rec.length));
  std::size_t covered = 0;
  while (covered < text_length + 1 && r.remaining() >= 10 && block.levels.size() < 10000) {
    const std::uint32_t count = r.u32le();
    const std::uint16_t level = r.u16le();
    const std::uint32_t mask = r.u32le();
    int bullet = -1;
    if (mask & 0x000F) {
      const std::uint16_t flags = r.u16le();
      if (mask & 1) bullet = (flags & 1) ? 1 : 0;
    }
    if (mask & 0x0080) r.skip(2);   // bullet char
    if (mask & 0x0010) r.skip(2);   // bullet font
    if (mask & 0x0040) r.skip(2);   // bullet size
    if (mask & 0x0020) r.skip(4);   // bullet colour
    if (mask & 0x0800) r.skip(2);   // alignment
    if (mask & 0x1000) r.skip(2);   // line spacing
    if (mask & 0x2000) r.skip(2);   // space before
    if (mask & 0x4000) r.skip(2);   // space after
    if (mask & 0x0100) r.skip(2);   // left margin
    if (mask & 0x0400) r.skip(2);   // indent
    if (mask & 0x8000) r.skip(2);   // default tab
    if (mask & 0x100000) {
      const std::uint16_t tabs = r.u16le();
      r.skip(std::size_t{tabs} * 4);
    }
    if (mask & 0x10000) r.skip(2);  // font align
    if (mask & 0xE0000) r.skip(2);  // wrap flags
    if (mask & 0x200000) r.skip(2); // text direction
    if (!r.ok()) break;
    // One entry per paragraph covered by this run.
    std::size_t paragraphs = 0;
    for (std::size_t k = covered; k < std::min<std::size_t>(covered + count, text_length); ++k) {
      if (block.text[k] == '\r') ++paragraphs;
    }
    if (covered + count >= text_length) ++paragraphs;
    for (std::size_t k = 0; k < std::max<std::size_t>(1, paragraphs); ++k) {
      block.levels.push_back(level);
      block.bullets.push_back(bullet);
    }
    covered += count;
  }
}

class PptReader {
 public:
  PptReader(const ReadContext& ctx, ReadSettings settings) : ctx_(ctx), settings_(settings) {}
  Result<ir::Document> read(const table::CfbReader& cfb);

 private:
  void walk(std::size_t at, std::size_t end, std::vector<TextBlock>& out, std::vector<ir::EncodedImage>& pictures,
            const std::vector<TextBlock>* outline, int depth);
  std::optional<std::size_t> persist_offset(std::uint32_t id) const {
    const auto it = persist_.find(id);
    return it == persist_.end() ? std::nullopt : std::optional<std::size_t>(it->second);
  }
  void load_blip_store(std::size_t at, std::size_t end, int depth);

  const ReadContext& ctx_;
  ReadSettings settings_;
  ByteView doc_;
  Bytes doc_bytes_;
  Bytes pictures_stream_;
  std::map<std::uint32_t, std::size_t> persist_;
  std::vector<std::uint32_t> blip_offsets_;  // BStore index -> offset in the Pictures stream
  std::size_t steps_ = 0;
};

std::string decode_chars(ByteView data, const Record& rec, bool bytes) {
  if (bytes) {
    return table::decode_code_page(data.subspan(rec.body, rec.length), *table::find_code_page(1252));
  }
  std::u16string text;
  for (std::size_t k = 0; k + 1 < rec.length; k += 2) text.push_back(static_cast<char16_t>(le16(data, rec.body + k)));
  return util::utf16_to_utf8(text);
}

void PptReader::load_blip_store(std::size_t at, std::size_t end, int depth) {
  if (depth > 8) return;
  while (at + 8 <= end) {
    const auto rec = record_at(doc_, at);
    if (!rec || ++steps_ > 1'000'000) return;
    if (rec->type == 0xF007) {  // FBSE
      blip_offsets_.push_back(rec->length >= 36 ? le32(doc_, rec->body + 28) : 0xFFFFFFFFu);
    } else if (rec->ver == 0x0F) {
      load_blip_store(rec->body, rec->body + rec->length, depth + 1);
    }
    at = rec->body + rec->length;
  }
}

void PptReader::walk(std::size_t at, std::size_t end, std::vector<TextBlock>& out, std::vector<ir::EncodedImage>& pictures,
                     const std::vector<TextBlock>* outline, int depth) {
  if (depth > 16) return;
  int pending_type = 4;
  while (at + 8 <= end) {
    const auto rec = record_at(doc_, at);
    if (!rec || ++steps_ > 2'000'000) return;
    switch (rec->type) {
      case 0x0F9F:  // TextHeaderAtom
        pending_type = static_cast<int>(le32(doc_, rec->body));
        break;
      case 0x0FA0:    // TextCharsAtom
      case 0x0FA8: {  // TextBytesAtom
        TextBlock block;
        block.type = pending_type;
        block.text = decode_chars(doc_, *rec, rec->type == 0x0FA8);
        // A StyleTextPropAtom may follow.
        if (const auto next = record_at(doc_, rec->body + rec->length); next && next->type == 0x0FA1) {
          parse_style(doc_, *next, block.text.size(), block);
        }
        out.push_back(std::move(block));
        break;
      }
      case 0x0F9E: {  // OutlineTextRefAtom: text stored in the slide list
        const std::uint32_t index = le32(doc_, rec->body);
        if (outline && index < outline->size()) out.push_back((*outline)[index]);
        break;
      }
      case 0xF00B: {  // OfficeArtFOPT: a picture reference (pib)
        const std::uint32_t count = rec->inst;
        for (std::uint32_t k = 0; k < count && 6 * k + 6 <= rec->length; ++k) {
          const std::uint16_t id = le16(doc_, rec->body + 6 * k);
          if ((id & 0x3FFF) == 0x0104) {
            const std::uint32_t pib = le32(doc_, rec->body + 6 * k + 2);
            if (pib >= 1 && pib <= blip_offsets_.size()) {
              const std::uint32_t offset = blip_offsets_[pib - 1];
              if (offset < pictures_stream_.size()) {
                if (auto image = find_blip(pictures_stream_, offset, pictures_stream_.size())) pictures.push_back(std::move(*image));
              }
            }
          }
        }
        break;
      }
      default:
        if (rec->ver == 0x0F) walk(rec->body, rec->body + rec->length, out, pictures, outline, depth + 1);
        break;
    }
    at = rec->body + rec->length;
  }
}

ir::Blocks text_blocks(const std::vector<TextBlock>& blocks, std::string& title, bool notes_only) {
  FlowBuilder flow;
  for (const TextBlock& block : blocks) {
    const bool is_title = block.type == 0 || block.type == 6;
    if (notes_only != (block.type == 2)) continue;
    if (is_title) {
      if (title.empty()) {
        std::string t = block.text;
        for (char& c : t) {
          if (c == '\r' || c == '\x0B') c = ' ';
        }
        title = std::string(util::trim(t));
        continue;
      }
    }
    const bool body = block.type == 1 || block.type == 5 || block.type == 7 || block.type == 8;
    std::size_t paragraph = 0;
    for (std::string_view line : util::split(block.text, '\r')) {
      std::string text(line);
      for (char& c : text) {
        if (c == '\x0B') c = '\n';
      }
      ir::Inlines content = text_with_breaks(util::trim(text));
      const int level = paragraph < block.levels.size() ? block.levels[paragraph] : 0;
      const int bullet = paragraph < block.bullets.size() ? block.bullets[paragraph] : -1;
      ++paragraph;
      if (content.empty()) continue;
      const bool listed = bullet == 1 || (bullet == -1 && body);
      if (listed) {
        FlowBuilder::ListInfo info;
        info.list_id = "body";
        info.level = std::clamp(level, 0, 8);
        flow.list_item(info, ir::Paragraph{std::move(content)});
      } else {
        flow.block(ir::Paragraph{std::move(content)});
      }
    }
    flow.close_lists();
  }
  return flow.take();
}

Result<ir::Document> PptReader::read(const table::CfbReader& cfb) {
  CVT_TRY_ASSIGN(doc_bytes_, cfb.read("PowerPoint Document"));
  doc_ = doc_bytes_;
  CVT_TRY_ASSIGN(const Bytes user, cfb.read("Current User"));
  if (cfb.contains("Pictures")) {
    if (auto p = cfb.read("Pictures")) pictures_stream_ = std::move(*p);
  }
  // CurrentUserAtom: header (8), size (4), headerToken (4), offsetToCurrentEdit (4).
  if (user.size() < 20) return fail(Status::Decode, "the Current User stream is truncated");
  const std::uint32_t token = le32(user, 12);
  if (token == 0xF3D1C4DFu) return fail(Status::Decode, "the presentation is password-protected; it cannot be read");
  std::uint32_t edit = le32(user, 16);
  // Follow the UserEditAtom chain (newest first) and merge persist directories.
  std::set<std::uint32_t> seen;
  std::uint32_t doc_persist = 0;
  while (edit != 0 && seen.insert(edit).second && seen.size() < 512) {
    const auto rec = record_at(doc_, edit);
    if (!rec || rec->type != 0x0FF5 || rec->length < 28) return fail(Status::Decode, "bad UserEditAtom");
    const std::uint32_t previous = le32(doc_, rec->body + 8);
    const std::uint32_t persist_dir = le32(doc_, rec->body + 12);
    if (const auto dir = record_at(doc_, persist_dir); dir && dir->type == 0x1772) {
      util::ByteReader r(doc_.subspan(dir->body, dir->length));
      while (r.remaining() >= 4) {
        const std::uint32_t header = r.u32le();
        const std::uint32_t start = header & 0xFFFFF;
        const std::uint32_t count = header >> 20;
        for (std::uint32_t k = 0; k < count && r.remaining() >= 4; ++k) persist_.emplace(start + k, r.u32le());
      }
    }
    edit = previous;
  }
  // docPersistIdRef is at offset 16 of the UserEditAtom body.
  {
    const auto rec = record_at(doc_, le32(user, 16));
    if (rec) doc_persist = le32(doc_, rec->body + 16);
  }
  const auto doc_offset = persist_offset(doc_persist);
  if (!doc_offset) return fail(Status::Decode, "the presentation has no document record");
  const auto document = record_at(doc_, *doc_offset);
  if (!document || document->type != 0x03E8) return fail(Status::Decode, "bad DocumentContainer");

  // Slide list (instance 0) and notes list (instance 2); the BLIP store.
  struct SlideRef {
    std::uint32_t persist = 0;
    std::uint32_t slide_id = 0;
    std::vector<TextBlock> outline;
  };
  std::vector<SlideRef> slides;
  std::map<std::uint32_t, std::uint32_t> notes_by_id;  // notes id -> persist id
  std::size_t at = document->body;
  const std::size_t end = document->body + document->length;
  while (at + 8 <= end) {
    const auto rec = record_at(doc_, at);
    if (!rec) break;
    if (rec->type == 0x0FF0) {  // SlideListWithText
      std::size_t p = rec->body;
      const std::size_t q = rec->body + rec->length;
      int pending_type = 4;
      SlideRef* current = nullptr;
      while (p + 8 <= q) {
        const auto inner = record_at(doc_, p);
        if (!inner) break;
        if (inner->type == 0x03F3) {  // SlidePersistAtom
          if (rec->inst == 0) {
            slides.push_back(SlideRef{le32(doc_, inner->body), le32(doc_, inner->body + 12), {}});
            current = &slides.back();
          } else if (rec->inst == 2) {
            notes_by_id[le32(doc_, inner->body + 12)] = le32(doc_, inner->body);
            current = nullptr;
          }
        } else if (inner->type == 0x0F9F) {
          pending_type = static_cast<int>(le32(doc_, inner->body));
        } else if ((inner->type == 0x0FA0 || inner->type == 0x0FA8) && current) {
          TextBlock block;
          block.type = pending_type;
          block.text = decode_chars(doc_, *inner, inner->type == 0x0FA8);
          if (const auto next = record_at(doc_, inner->body + inner->length); next && next->type == 0x0FA1) {
            parse_style(doc_, *next, block.text.size(), block);
          }
          current->outline.push_back(std::move(block));
        }
        p = inner->body + inner->length;
      }
    } else if (rec->type == 0x040B) {  // PPDrawingGroup
      load_blip_store(rec->body, rec->body + rec->length, 0);
    }
    at = rec->body + rec->length;
  }
  CVT_TRY(ctx_.limits.check_archive_entries(slides.size()));

  ir::Document doc;
  for (std::size_t i = 0; i < slides.size(); ++i) {
    CVT_TRY(ctx_.checkpoint(static_cast<double>(i) / static_cast<double>(slides.size()), "Reading slides"));
    std::vector<TextBlock> texts;
    std::vector<ir::EncodedImage> pictures;
    std::uint32_t notes_id = 0;
    if (const auto off = persist_offset(slides[i].persist)) {
      if (const auto slide = record_at(doc_, *off); slide && slide->type == 0x03EE) {
        walk(slide->body, slide->body + slide->length, texts, pictures, &slides[i].outline, 0);
        if (const auto atom = record_at(doc_, slide->body); atom && atom->type == 0x03EF && atom->length >= 20) {
          notes_id = le32(doc_, atom->body + 16);
        }
      }
    }
    if (texts.empty()) texts = slides[i].outline;
    std::string title;
    ir::Blocks body = text_blocks(texts, title, false);
    for (ir::EncodedImage& picture : pictures) body.push_back(ir::ImageBlock{std::move(picture), {}, {}});
    ir::Blocks notes;
    if (settings_.notes && notes_id != 0) {
      if (const auto n = notes_by_id.find(notes_id); n != notes_by_id.end()) {
        if (const auto off = persist_offset(n->second)) {
          if (const auto rec = record_at(doc_, *off); rec && rec->type == 0x03F0) {
            std::vector<TextBlock> note_texts;
            std::vector<ir::EncodedImage> ignored;
            walk(rec->body, rec->body + rec->length, note_texts, ignored, nullptr, 0);
            std::string unused;
            notes = text_blocks(note_texts, unused, true);
          }
        }
      }
    }
    if (!doc.blocks.empty()) doc.blocks.push_back(ir::PageBreak{});
    for (ir::Block& b : make_slide(title, std::move(body), std::move(notes))) doc.blocks.push_back(std::move(b));
  }
  // Document properties: the title only unless metadata is kept.
  if (auto summary = cfb.read("\x05SummaryInformation")) {
    const ByteView s = *summary;
    const std::uint32_t section = le32(s, 44);
    const std::uint32_t count = section < s.size() ? le32(s, section + 4) : 0;
    for (std::uint32_t k = 0; k < count && k < 256; ++k) {
      const std::uint32_t id = le32(s, section + 8 + 8 * k);
      const std::size_t prop = static_cast<std::size_t>(section) + le32(s, section + 12 + 8 * k);
      if (prop + 8 > s.size() || le32(s, prop) != 30) continue;
      const std::uint32_t length = le32(s, prop + 4);
      if (length == 0 || prop + 8 + length > s.size()) continue;
      std::string value(as_chars(s.subspan(prop + 8, length)));
      value = table::decode_code_page(as_bytes(value.substr(0, value.find('\0'))), *table::find_code_page(1252));
      if (id == 2) doc.metadata.title = value;
      if (id == 4) doc.metadata.author = value;
    }
  }
  finish_metadata(doc, settings_);
  return doc;
}

Result<AnyIR> read_ppt(ReadContext& ctx) {
  CVT_TRY_ASSIGN(const table::CfbReader cfb, table::CfbReader::open(ctx.input, ctx.limits));
  if (cfb.contains("EncryptedPackage")) {
    return fail(Status::Decode, "the presentation is password-protected (encrypted OOXML); it cannot be read");
  }
  if (!cfb.contains("PowerPoint Document")) return fail(Status::Decode, "the compound file is not a PowerPoint presentation");
  PptReader reader(ctx, read_settings(ctx.options));
  CVT_TRY_ASSIGN(ir::Document doc, reader.read(cfb));
  return AnyIR(std::move(doc));
}

}  // namespace

void register_ppt(Registry& registry) {
  FormatInfo info;
  info.id = "ppt";
  info.name = "PowerPoint 97-2003 presentation";
  info.description = "Microsoft PowerPoint binary presentation (PowerPoint 97 to 2003).";
  info.category = Category::Presentation;
  info.extensions = {"ppt", "pps", "pot"};
  info.mime = "application/vnd.ms-powerpoint";
  info.popularity = 45;
  info.sniff = [](ByteView data, std::string_view) { return compound_file_has(data, u"PowerPoint Document") ? 97 : 0; };
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = "ppt";
  codec.kind = Kind::Document;
  codec.read = read_ppt;
  codec.read_options = document_read_options();
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::office
