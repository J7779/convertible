// The multi-file queue (cards, the "All N files to…" control, remove, clear)
// and the target picker (grouping, search, keyboard, dismissal).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cctype>
#include <set>

#include "common.hpp"

using namespace e2e;

namespace {

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> flat_targets(const Json& picker) {
  std::vector<std::string> out;
  for (const Json& g : picker["groups"].items()) {
    for (const Json& t : g["targets"].items()) out.push_back(t.str());
  }
  return out;
}

}  // namespace

TEST_CASE("e2e/queue/one control sets the target of every file of a format") {
  Session s("queue-group");
  s.write_input("a.txt", "alpha\n");
  s.write_input("b.txt", "beta\n");
  s.write_input("c.json", "{\"c\": true}\n");
  REQUIRE(s.ok(s.run("queue_group.json")));

  const Json q0 = s.state("q0.json");
  REQUIRE(q0["items"].size() == 3u);
  const Json* group = nullptr;
  for (const Json& n : q0["accessibility"].items()) {
    if (n["label"].str().rfind("All 2 TXT files to", 0) == 0) group = &n;
  }
  REQUIRE(group != nullptr);
  CHECK((*group)["role"].str() == "button");
  // Both share the default target (the core's first same-category target, so
  // whatever the catalogue ranks first; never a hard-coded id).
  REQUIRE(at(q0, "items.0.target").str() == at(q0, "items.1.target").str());
  CHECK((*group)["value"].str() == upper(at(q0, "items.0.target").str()));

  const Json picker = s.state("q-picker.json");
  CHECK(picker["picker"]["open"].as_bool());
  CHECK(picker["picker"]["groupSource"].str() == "txt");

  const Json q1 = s.state("q1.json");
  CHECK_FALSE(q1["picker"]["open"].as_bool());
  CHECK(at(q1, "items.0.target").str() == "hex");
  CHECK(at(q1, "items.1.target").str() == "hex");
  CHECK(at(q1, "items.2.target").str() == at(q0, "items.2.target").str());  // the JSON file is untouched
  // Options follow the new target's schema.
  CHECK(at(q1, "items.0.options").contains("uppercase"));

  const Json q2 = s.state("q2.json");
  bool mixed = false;
  for (const Json& n : q2["accessibility"].items()) {
    mixed = mixed || (n["label"].str().rfind("All 2 TXT files to", 0) == 0 && n["value"].str() == "Mixed");
  }
  CHECK(mixed);

  const Json q3 = s.state("q3.json");
  REQUIRE(q3["items"].size() == 2u);
  CHECK(at(q3, "items.0.name").str() == "a.txt");
  CHECK(at(q3, "items.1.name").str() == "c.json");
  bool still_grouped = false;
  for (const Json& n : q3["accessibility"].items()) still_grouped = still_grouped || n["label"].str().find("files to") != std::string::npos;
  CHECK_FALSE(still_grouped);  // one TXT file left: no group control

  const Json q4 = s.state("q4.json");
  CHECK(q4["items"].size() == 0u);
  CHECK(node(q4, "Choose files to convert", "button") != nullptr);  // back to the empty state
}

TEST_CASE("e2e/picker/grouping, search, keyboard navigation and dismissal") {
  Session s("picker");
  s.write_input("notes.txt", "Some notes.\nTwo lines.\n");
  REQUIRE(s.ok(s.run("picker.json")));

  const Json closed = s.state("p-closed0.json");
  const Json& all_ids = at(closed, "items.0.targetIds");
  const std::string initial = at(closed, "items.0.target").str();
  REQUIRE(all_ids.size() > 3u);

  // Open: grouped by category, the source's own category first, nothing lost or duplicated.
  const Json open = s.state("p-open.json");
  REQUIRE(open["picker"]["open"].as_bool());
  const Json& picker = open["picker"];
  const auto flat = flat_targets(picker);
  CHECK(flat.size() == all_ids.size());
  CHECK(std::set<std::string>(flat.begin(), flat.end()).size() == flat.size());
  for (const auto& id : flat) CHECK(contains(all_ids, id));
  std::set<std::string> cats;
  for (const Json& g : picker["groups"].items()) {
    CHECK(cats.insert(g["category"].str()).second);  // each category once
  }
  CHECK(at(picker, "groups.0.category").str() == picker["sourceCategory"].str());
  // Within a group the core's relevance order is kept.
  for (const Json& g : picker["groups"].items()) {
    std::size_t last = 0;
    for (const Json& t : g["targets"].items()) {
      std::size_t pos = 0;
      while (pos < all_ids.size() && all_ids[pos].str() != t.str()) ++pos;
      CHECK(pos >= last);
      last = pos;
    }
  }
  // The highlight starts on the current target; a search field has focus.
  CHECK(flat[static_cast<std::size_t>(picker["highlight"].as_int())] == initial);
  CHECK(node(open, "Search formats", "textfield") != nullptr);
  CHECK(node(open, "Choose a target format", "group") != nullptr);
  CHECK(nodes_with_role(open, "menuitem").size() > 3u);

  // Search narrows the list.
  const Json search = s.state("p-search.json");
  CHECK(search["picker"]["query"].str() == "hex");
  const auto found = flat_targets(search["picker"]);
  CHECK(!found.empty());
  CHECK(found.size() < flat.size());
  CHECK(std::find(found.begin(), found.end(), "hex") != found.end());

  // Escape closes without changing anything.
  const Json escaped = s.state("p-escaped.json");
  CHECK_FALSE(escaped["picker"]["open"].as_bool());
  CHECK(at(escaped, "items.0.target").str() == initial);

  // Down x3 then Enter picks the highlighted entry.
  const Json nav = s.state("p-nav.json");
  const auto nav_flat = flat_targets(nav["picker"]);
  const int h = nav["picker"]["highlight"].as_int();
  const auto start = static_cast<int>(std::find(nav_flat.begin(), nav_flat.end(), initial) - nav_flat.begin());
  CHECK(h == std::min(start + 3, static_cast<int>(nav_flat.size()) - 1));
  const Json chosen = s.state("p-chosen.json");
  CHECK_FALSE(chosen["picker"]["open"].as_bool());
  CHECK(at(chosen, "items.0.target").str() == nav_flat[static_cast<std::size_t>(h)]);

  // Page Down jumps eight rows; a query without matches shows nothing and Enter does nothing.
  const int reopen_h = s.state("p-reopen.json")["picker"]["highlight"].as_int();
  CHECK(s.state("p-page.json")["picker"]["highlight"].as_int() ==
        std::min(reopen_h + 8, static_cast<int>(nav_flat.size()) - 1));
  const Json none = s.state("p-none.json");
  CHECK(none["picker"]["open"].as_bool());
  CHECK(none["picker"]["groups"].size() == 0u);
  CHECK(at(none, "items.0.target").str() == at(chosen, "items.0.target").str());
  // A click outside closes it.
  CHECK_FALSE(s.state("p-outside.json")["picker"]["open"].as_bool());
}
