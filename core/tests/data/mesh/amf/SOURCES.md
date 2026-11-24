# Sources: mesh/amf

Written by tools/scene_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cubes_hand.amf` | cube with vertex colours and material Red, placed twice by a constellation (one rotated 90 degrees) | Python, written per ISO/ASTM 52915 (AMF 1.1); assimp loads it (12 faces, its importer ignores constellations) | 24 triangles, bounds, material, colours, objects |
| `cubes_hand_zipped.amf` | the same document in a ZIP | Python zipfile | zipped AMF |
