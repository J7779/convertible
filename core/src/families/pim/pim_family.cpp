// Convertible core - pim family.
// Personal information formats: calendars (ics, vcs, calendar CSV), contacts
// (vcf, jCard, xCard, LDIF, contacts CSV), playlists, bookmarks and link files,
// news feeds, bibliographies and e-mail messages.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/pim/pim.hpp"

namespace cvt {

void register_pim_family(Registry& registry) {
  pim::register_calendars(registry);
  pim::register_contacts(registry);
  pim::register_pim_bridges(registry);
  pim::register_playlists(registry);
  pim::register_bookmarks(registry);
  pim::register_feeds(registry);
  pim::register_bibliographies(registry);
  pim::register_mail(registry);

  // Playlists, cue sheets and feeds have a schema of their own (a location
  // column, a title and items): their writers take what their readers make
  // (Codec::ir_tag), so a CSV or a JSON file is not offered as a playlist.
  registry.set_ir_tag({"m3u", "m3u8", "pls", "asx", "xspf", "wpl", "cue"}, Kind::Data, "playlist");
  registry.set_ir_tag({"m3u", "m3u8", "pls", "asx", "xspf", "wpl", "cue"}, Kind::Table, "playlist");
  registry.set_ir_tag({"atom", "rss", "json-feed"}, Kind::Data, "feed");
}

}  // namespace cvt
