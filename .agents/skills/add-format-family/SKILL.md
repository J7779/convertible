---
name: add-format-family
description: Start a new format family in the Convertible core (a new directory under core/src/families with its registration function, private header, optional family.cmake, tests and fixture directory), or choose between a new family and an existing one. Use when a group of related formats has no owner yet.
---

# Add a format family

A family is a directory that owns a group of formats end to end. Most new formats belong in
an existing family (`add-format` skill); create a family only when no row of the table in
`core/README.md` ("Who owns what") fits.

## Steps

1. **Agree on the family** with the maintainers first: its name (lower-case, `snake_case`),
   the formats it owns, the IR kind(s) it reads and writes, and the bridges it owns (a bridge
   belongs to the family that owns its source kind). Small families reuse existing kinds (a
   playlist is a `Table`); a new kind in `src/ir/` is a shared-interface change.
2. **Hook it in.** The maintainers add one declaration to `core/src/families/families.hpp`
   and one call to `core/src/families/register_all.cpp` (shared files: family branches do
   not edit them). Registration order there decides tie-breaks, so new families go last.
3. **Create the directory** `core/src/families/<family>/`:
   - `<family>_family.cpp` with `void register_<family>_family(Registry& registry)`
     (pure: no I/O, no globals), calling one `register_<format>()` per format;
   - `<family>.hpp`, the family's private header declaring those functions;
   - optional `family.cmake` (included automatically) for compile definitions or imported
     code (`import-codec` skill). It must never download or look anything up.
   Header of every file: `// Copyright (c) Convertible contributors. MIT licence; see LICENSE.`
4. **Tests and fixtures**: `core/tests/families/<family>/` (picked up by glob) and
   `core/tests/data/<family>/` with a `SOURCES.md`. Test names start with `"<family>/"`.
5. **Add the formats** one by one with the `add-format` skill.
6. **Detection rows** for formats the family will own later can be appended to
   `core/src/engine/known_formats.cpp` (one line each); a family registration for the same id
   wins field by field.
7. **Check**:
   ```sh
   cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
   build/dev/core/convertible formats --category <category>
   tools/gen-formats-doc
   tools/ci/docker-linux.sh gcc
   ```
   If the family adds a new category name, `tools/gen-formats-doc` lists it automatically;
   add a display title to its `CATEGORIES` table for a nicer heading.
