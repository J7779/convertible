// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <utility>

#include "families/families.hpp"
#include "util/hex.hpp"
#include "util/text.hpp"

namespace cvt {

namespace {

constexpr std::array<std::pair<Category, std::string_view>, 18> category_names = {{
    {Category::Image, "image"},
    {Category::Audio, "audio"},
    {Category::Video, "video"},
    {Category::Document, "document"},
    {Category::Ebook, "ebook"},
    {Category::Spreadsheet, "spreadsheet"},
    {Category::Presentation, "presentation"},
    {Category::Data, "data"},
    {Category::Archive, "archive"},
    {Category::Font, "font"},
    {Category::Subtitle, "subtitle"},
    {Category::Model3D, "model3d"},
    {Category::Geo, "geo"},
    {Category::Pim, "pim"},
    {Category::Code, "code"},
    {Category::Encoding, "encoding"},
    {Category::Vector, "vector"},
    {Category::Other, "other"},
}};

bool valid_id(std::string_view id) {
  if (id.empty() || id.size() > 32) return false;
  return std::ranges::all_of(id, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '+';
  });
}

std::string normalize_extension(std::string_view extension) {
  std::string out = util::to_lower(util::trim(extension));
  while (!out.empty() && out.front() == '.') out.erase(out.begin());
  return out;
}

void add_unique(std::vector<std::string>& list, std::string value) {
  if (value.empty() || std::ranges::find(list, value) != list.end()) return;
  list.push_back(std::move(value));
}

bool by_popularity(const FormatInfo* a, const FormatInfo* b) {
  if (a->popularity != b->popularity) return a->popularity > b->popularity;
  return a->id < b->id;
}

}  // namespace

std::string_view category_name(Category category) noexcept {
  for (const auto& [value, name] : category_names) {
    if (value == category) return name;
  }
  return "other";
}

std::optional<Category> parse_category(std::string_view name) noexcept {
  for (const auto& [value, text] : category_names) {
    if (util::iequals(text, name)) return value;
  }
  return std::nullopt;
}

std::string normalize_format_id(std::string_view text) {
  std::string out = util::to_lower(util::trim(text));
  if (out.size() > 1 && out.front() == '.') out.erase(out.begin());
  return out;
}

// --------------------------------------------------------------------------- Signature

std::optional<Signature> Signature::parse(std::size_t offset, std::string_view pattern) {
  if (detail::signature_pattern_length(pattern) < 0) return std::nullopt;
  Signature signature;
  signature.offset = offset;
  bool masked = false;
  std::size_t i = 0;
  const auto nibble = [](char c) { return c == '?' ? 0 : util::hex_digit_value(c); };
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (c == ' ') {
      ++i;
    } else if (c == '\'') {
      const std::size_t close = pattern.find('\'', i + 1);
      for (std::size_t k = i + 1; k < close; ++k) {
        signature.bytes.push_back(static_cast<std::byte>(pattern[k]));
        signature.mask.push_back(std::byte{0xFF});
      }
      i = close + 1;
    } else {
      const char high = c;
      const char low = pattern[i + 1];
      auto value = static_cast<std::uint8_t>((nibble(high) << 4) | nibble(low));
      auto mask = static_cast<std::uint8_t>((high == '?' ? 0x00 : 0xF0) | (low == '?' ? 0x00 : 0x0F));
      i += 2;
      if (i < pattern.size() && pattern[i] == '&') {
        mask = static_cast<std::uint8_t>((util::hex_digit_value(pattern[i + 1]) << 4) |
                                         util::hex_digit_value(pattern[i + 2]));
        i += 3;
      }
      value &= mask;
      if (mask != 0xFF) masked = true;
      signature.bytes.push_back(static_cast<std::byte>(value));
      signature.mask.push_back(static_cast<std::byte>(mask));
    }
  }
  if (!masked) signature.mask.clear();
  return signature;
}

Signature Signature::at(std::size_t offset, SignaturePattern pattern) {
  // The pattern was validated at compile time; an empty one yields a
  // signature that never matches.
  return parse(offset, pattern.text).value_or(Signature{offset, {}, {}});
}

bool Signature::matches(ByteView data) const noexcept {
  if (bytes.empty() || offset > data.size() || bytes.size() > data.size() - offset) return false;
  if (!mask.empty() && mask.size() != bytes.size()) return false;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const std::byte m = mask.empty() ? std::byte{0xFF} : mask[i];
    if ((data[offset + i] & m) != (bytes[i] & m)) return false;
  }
  return true;
}

std::size_t Signature::significant_bits() const noexcept {
  if (mask.empty()) return bytes.size() * 8;
  std::size_t bits = 0;
  for (const std::byte m : mask) bits += static_cast<std::size_t>(std::popcount(std::to_integer<std::uint8_t>(m)));
  return bits;
}

// --------------------------------------------------------------------------- Registry

const Registry& Registry::instance() {
  static const Registry shared = [] {
    Registry registry;
    register_all(registry);
    registry.finalize();
    return registry;
  }();
  return shared;
}

Registry::Entry& Registry::entry_for(std::string id) {
  auto it = formats_.find(id);
  if (it == formats_.end()) {
    Entry entry;
    entry.info.id = id;
    it = formats_.emplace(std::move(id), std::move(entry)).first;
  }
  return it->second;
}

void Registry::add_format(FormatInfo info) {
  info.id = normalize_format_id(info.id);
  if (info.id.empty()) return;
  Entry& entry = entry_for(info.id);
  FormatInfo& target = entry.info;
  // An authoritative description replaces whatever the known-formats
  // database supplied; database values only survive where this call is silent.
  if (!info.name.empty()) target.name = std::move(info.name);
  if (!info.description.empty()) target.description = std::move(info.description);
  if (info.category != Category::Other) target.category = info.category;
  if (!info.mime.empty()) target.mime = std::move(info.mime);
  if (info.popularity != 0) target.popularity = std::clamp(info.popularity, 0, 100);
  if (!info.extensions.empty()) {
    // This call's extensions lead (its first one becomes the default for
    // output files); previously known ones follow.
    std::vector<std::string> merged;
    for (const std::string& extension : info.extensions) add_unique(merged, normalize_extension(extension));
    for (std::string& extension : target.extensions) add_unique(merged, std::move(extension));
    target.extensions = std::move(merged);
  }
  if (!entry.authoritative && (!info.signatures.empty() || info.sniff)) {
    // The family says how to recognise the format: whatever the database
    // contributed (in either registration order) no longer applies.
    target.signatures.clear();
    target.sniff = nullptr;
  }
  for (Signature& signature : info.signatures) target.signatures.push_back(std::move(signature));
  if (info.sniff) target.sniff = std::move(info.sniff);
  entry.authoritative = true;
}

void Registry::add_known_format(FormatInfo info) {
  info.id = normalize_format_id(info.id);
  if (info.id.empty()) return;
  Entry& entry = entry_for(info.id);
  FormatInfo& target = entry.info;
  if (target.name.empty()) target.name = std::move(info.name);
  if (target.description.empty()) target.description = std::move(info.description);
  if (target.category == Category::Other) target.category = info.category;
  if (target.mime.empty()) target.mime = std::move(info.mime);
  if (target.popularity == 0) target.popularity = std::clamp(info.popularity, 0, 100);
  for (const std::string& extension : info.extensions) add_unique(target.extensions, normalize_extension(extension));
  // Database signatures only fill a gap: once a family has described how to
  // recognise the format (signatures or sniff), its word is final.
  const bool family_detects = entry.authoritative && (!target.signatures.empty() || target.sniff);
  if (!family_detects) {
    for (Signature& signature : info.signatures) target.signatures.push_back(std::move(signature));
    if (!target.sniff && info.sniff) target.sniff = std::move(info.sniff);
  }
}

void Registry::add_codec(Codec codec) {
  codec.format_id = normalize_format_id(codec.format_id);
  codecs_by_format_[codec.format_id].push_back(codecs_.size());
  codecs_.push_back(std::move(codec));
}

void Registry::add_bridge(Bridge bridge) { bridges_.push_back(std::move(bridge)); }

void Registry::finalize() {
  const auto ensure = [this](const std::string& id) {
    if (id.empty()) return;
    Entry& entry = entry_for(id);
    if (entry.info.name.empty()) entry.info.name = util::to_upper(id);
    if (entry.info.extensions.empty()) entry.info.extensions.push_back(id);
  };
  for (const Codec& codec : codecs_) ensure(codec.format_id);
  for (auto& [id, entry] : formats_) {
    if (entry.info.name.empty()) entry.info.name = util::to_upper(id);
  }
}

const FormatInfo* Registry::find(std::string_view id) const {
  const auto it = formats_.find(id);
  return it == formats_.end() ? nullptr : &it->second.info;
}

const FormatInfo* Registry::resolve(std::string_view id_or_extension) const {
  const std::string key = normalize_format_id(id_or_extension);
  if (key.empty()) return nullptr;
  if (const FormatInfo* exact = find(key)) return exact;
  const auto candidates = formats_with_extension(key);
  return candidates.empty() ? nullptr : candidates.front();
}

std::vector<const FormatInfo*> Registry::formats() const {
  std::vector<const FormatInfo*> out;
  out.reserve(formats_.size());
  for (const auto& [id, entry] : formats_) out.push_back(&entry.info);
  std::ranges::sort(out, by_popularity);
  return out;
}

std::vector<const FormatInfo*> Registry::formats_with_extension(std::string_view extension) const {
  std::vector<const FormatInfo*> out;
  for (const auto& [id, entry] : formats_) {
    if (std::ranges::find(entry.info.extensions, extension) != entry.info.extensions.end()) out.push_back(&entry.info);
  }
  // A format whose primary extension matches outranks one that merely lists it.
  std::ranges::stable_sort(out, [&](const FormatInfo* a, const FormatInfo* b) {
    const bool a_primary = a->extensions.front() == extension;
    const bool b_primary = b->extensions.front() == extension;
    if (a_primary != b_primary) return a_primary;
    const bool a_family = described_by_family(a->id);
    const bool b_family = described_by_family(b->id);
    if (a_family != b_family) return a_family;
    return by_popularity(a, b);
  });
  return out;
}

bool Registry::described_by_family(std::string_view id) const {
  const auto it = formats_.find(id);
  return it != formats_.end() && it->second.authoritative;
}

std::vector<const Codec*> Registry::codecs_for(std::string_view format_id) const {
  std::vector<const Codec*> out;
  const auto it = codecs_by_format_.find(format_id);
  if (it == codecs_by_format_.end()) return out;
  out.reserve(it->second.size());
  for (const std::size_t index : it->second) out.push_back(&codecs_[index]);
  return out;
}

bool Registry::has_reader(std::string_view format_id) const {
  return std::ranges::any_of(codecs_for(format_id), [](const Codec* codec) { return static_cast<bool>(codec->read); });
}

bool Registry::has_writer(std::string_view format_id) const {
  return std::ranges::any_of(codecs_for(format_id), [](const Codec* codec) { return static_cast<bool>(codec->write); });
}

std::vector<std::string> Registry::validate() const {
  std::vector<std::string> problems;
  const auto problem = [&problems](std::string text) { problems.push_back(std::move(text)); };

  const auto check_specs = [&problem](const std::string& owner, const std::vector<OptionSpec>& specs) {
    for (const OptionSpec& spec : specs) {
      if (spec.key.empty() || spec.label.empty()) problem(std::format("{}: option without key or label", owner));
      if (spec.key == "limits") problem(std::format("{}: option key 'limits' is reserved", owner));
      if (spec.type == OptionType::Enum && spec.choices.empty()) {
        problem(std::format("{}: enum option '{}' has no choices", owner, spec.key));
      }
      if ((spec.type == OptionType::Int || spec.type == OptionType::Float) && spec.min && spec.max && *spec.min > *spec.max) {
        problem(std::format("{}: option '{}' has min > max", owner, spec.key));
      }
      Json given = Json::object();
      given[spec.key] = spec.default_value;
      const auto normalized = Options(given).normalized(std::span<const OptionSpec>(&spec, 1));
      if (spec.default_value.is_null() || !normalized || normalized->json()[spec.key] != spec.default_value) {
        problem(std::format("{}: default of option '{}' does not satisfy its own spec", owner, spec.key));
      }
    }
  };

  for (const auto& [id, entry] : formats_) {
    const FormatInfo& info = entry.info;
    if (!valid_id(id)) problem(std::format("format '{}': id must be lower-case [a-z0-9._+-], at most 32 characters", id));
    if (info.name.empty()) problem(std::format("format '{}': name is empty", id));
    if (info.popularity < 0 || info.popularity > 100) problem(std::format("format '{}': popularity out of 0..100", id));
    for (const std::string& extension : info.extensions) {
      if (extension.empty() || extension.front() == '.' || extension != util::to_lower(extension)) {
        problem(std::format("format '{}': extension '{}' must be lower-case without a dot", id, extension));
      }
    }
    for (const Signature& signature : info.signatures) {
      if (signature.bytes.empty()) problem(std::format("format '{}': empty signature", id));
      if (!signature.mask.empty() && signature.mask.size() != signature.bytes.size()) {
        problem(std::format("format '{}': signature mask length differs from pattern length", id));
      }
    }
  }
  // Two formats may share an extension only if at most one of them has no
  // way of telling by content; otherwise detection would be a coin toss.
  // A family's format and a detection-only database entry may share an
  // untested extension: the family's format wins (formats_with_extension and
  // detection rank it first), so only collisions within one group count.
  std::map<std::string, std::string, std::less<>> untested_owner;
  std::map<std::string, std::string, std::less<>> untested_known_owner;
  for (const auto& [id, entry] : formats_) {
    const FormatInfo& info = entry.info;
    if (!info.signatures.empty() || info.sniff) continue;
    if (!entry.authoritative) {
      for (const std::string& extension : info.extensions) {
        const auto [it, inserted] = untested_known_owner.emplace(extension, id);
        if (!inserted) {
          problem(std::format("formats '{}' and '{}' both claim extension '{}' without signatures or a sniff function",
                              it->second, id, extension));
        }
      }
      continue;
    }
    for (const std::string& extension : info.extensions) {
      const auto [it, inserted] = untested_owner.emplace(extension, id);
      if (!inserted) {
        problem(std::format("formats '{}' and '{}' both claim extension '{}' without signatures or a sniff function",
                            it->second, id, extension));
      }
    }
  }
  for (const Codec& codec : codecs_) {
    if (find(codec.format_id) == nullptr) problem(std::format("codec for unknown format '{}'", codec.format_id));
    if (!codec.read && !codec.write) {
      problem(std::format("codec {}/{} has neither read nor write", codec.format_id, kind_name(codec.kind)));
    }
    if (codec.read_cost <= 0 || codec.write_cost <= 0) {
      problem(std::format("codec {}/{} has a non-positive cost", codec.format_id, kind_name(codec.kind)));
    }
    const std::string owner = std::format("codec {}/{}", codec.format_id, kind_name(codec.kind));
    check_specs(owner, codec.read_options);
    check_specs(owner, codec.write_options);
  }
  // Ownership: one family owns each (format, kind) direction. Two readers (or
  // two writers) of the same kind for one format mean two families claim it,
  // and which one runs would depend on costs and registration order.
  {
    std::map<std::string, int, std::less<>> readers;
    std::map<std::string, int, std::less<>> writers;
    for (const Codec& codec : codecs_) {
      const std::string key = std::format("{}/{}", codec.format_id, kind_name(codec.kind));
      if (codec.read && ++readers[key] == 2) {
        problem(std::format("format '{}' has two {} readers: one family must own it", codec.format_id, kind_name(codec.kind)));
      }
      if (codec.write && ++writers[key] == 2) {
        problem(std::format("format '{}' has two {} writers: one family must own it", codec.format_id, kind_name(codec.kind)));
      }
    }
  }
  for (const Bridge& bridge : bridges_) {
    if (!bridge.fn) problem(std::format("bridge {} -> {} has no function", kind_name(bridge.from), kind_name(bridge.to)));
    if (bridge.from == bridge.to) problem(std::format("bridge {} -> itself", kind_name(bridge.from)));
    if (bridge.cost <= 0) {
      problem(std::format("bridge {} -> {} has a non-positive cost", kind_name(bridge.from), kind_name(bridge.to)));
    }
    check_specs(std::format("bridge {} -> {}", kind_name(bridge.from), kind_name(bridge.to)), bridge.options);
  }
  for (const std::string& issue : media_.problems()) problem("media codec registry: " + issue);
  return problems;
}

}  // namespace cvt
