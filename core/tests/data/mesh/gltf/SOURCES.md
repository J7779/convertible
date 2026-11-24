# Sources: mesh/gltf

Written by tools/blender_export.py (`export_scene.gltf`) and tools/scene_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.glb, star_blender.glb` | cube; star (triangulated by the exporter) | Blender 4.1.1 | counts, normals, area, metres |
| `quad_blender.glb` | quad with material Checker (roughness 0.5) and embedded PNG | Blender 4.1.1 | material, texture bytes, uv orientation |
| `two_objects_blender.glb` | two nodes with TRS, material Red (metallic 0.25, roughness 0.6) | Blender 4.1.1 | node transforms, shape, PBR values |
| `quad_pygltflib_embedded.gltf` | the quad GLB re-saved with data: URI buffers | pygltflib `convert_buffers(DATAURI)` | embedded buffers |
| `quad_pygltflib_separate.gltf` | the quad with its buffer pointing at a quad.bin that is not supplied | pygltflib | clear warning + decode error |
| `edge_cases_pygltflib.gltf` | byteStride 24 interleaved positions+normals, UNSIGNED_BYTE indices, triangle strip and fan, normalized UNSIGNED_SHORT uvs, sparse accessor, TRS node, matrix parent | pygltflib (built accessor by accessor) | 6 triangles at the expected world positions, winding |
| `sphere_trimesh_colors.glb` | sphere with COLOR_0 as normalized bytes (255, 128, 0) | trimesh | colours |
| `two_objects_assimp.glb` | the two-object OBJ converted | assimp `export -fglb2 -tri` | 236 triangles, bounds |
