# Sources: mesh/3mf

Written by tools/scene_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `two_objects_assimp.3mf` | the two-object OBJ (triangulated) | assimp `export -f3mf -tri` | 236 triangles, Z-up conversion |
| `two_objects_trimesh.3mf` | two build items with transforms | trimesh `Scene.export(file_type='3mf')` | objects, bounds |
| `components_materials_hand.3mf` | 10 mm cubes: an object made of two components (one translated 20 mm), a rotated build item, base materials Red/Blue (#RRGGBBAA), a colour group, a Title | Python zipfile, model XML written per the 3MF core and materials specifications; assimp loads it | units, transforms (bounds), materials, colours, metadata |
