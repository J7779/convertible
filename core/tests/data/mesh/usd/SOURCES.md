# Sources: mesh/usd

Written by tools/blender_export.py (`wm.usd_export`) and tools/usd_x3d_fixtures.py; see `../SOURCES.md` for the container and the commands.

| File | Contents | Tool | Checked by the tests |
| --- | --- | --- | --- |
| `cube_blender.usda, quad_blender.usda, two_objects_blender.usda` | Z-up stage in metres, faceVarying normals/uvs, primvars:Col, UsdPreviewSurface + UsdUVTexture | Blender 4.1.1 | axes, units, transforms, materials, primvars |
| `two_objects_usdcore.usda` | Z-up stage in centimetres, translate/rotateXYZ/scale ops, indexed faceVarying st, constant displayColor, preview material Green | usd-core 26.11 (pxr.Usd/UsdGeom/UsdShade) | units, xform ops, material values, indexed primvars |
| `two_objects_usdcore.usdz` | the same stage with a textured material, packaged with its PNG | usd-core `UsdUtils.CreateNewUsdzPackage` | usda root layer, texture bytes |
| `cube_crate.usdz` | cube exported to a binary crate layer and packaged | usd-core (`Stage.Export` to .usdc + `CreateNewUsdzPackage`) | clear unsupported-usdc error |
