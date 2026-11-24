# glTF variants, 3MF and AMF fixtures (run from tests/data/mesh after
# tools/blender_export.py):
#   /venv/bin/python tools/scene_fixtures.py
import os
import shutil
import struct
import subprocess
import sys
import zipfile

import numpy as np
import pygltflib as G
import trimesh

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_np as S  # noqa: E402

for d in ("gltf", "3mf", "amf"):
    os.makedirs(d, exist_ok=True)

# --- glTF ------------------------------------------------------------------
# Blender's textured quad as .gltf with embedded data: URIs (pygltflib), and
# as a .gltf whose buffer is a separate .bin that is NOT supplied.
q = G.GLTF2().load("gltf/quad_blender.glb")
q.convert_buffers(G.BufferFormat.DATAURI)
q.save("gltf/quad_pygltflib_embedded.gltf")
os.makedirs("/tmp/sep", exist_ok=True)
q = G.GLTF2().load("gltf/quad_blender.glb")
q.buffers[0].uri = "quad.bin"  # the .bin is deliberately not supplied
q._glb_data = None
q.save_json("/tmp/sep/quad.gltf")
with open("/tmp/sep/quad.gltf") as f:
    text = f.read()
with open("gltf/quad_pygltflib_separate.gltf", "w") as f:
    f.write(text)

# Hand-built edge cases with pygltflib: interleaved positions+normals
# (byteStride 24), UNSIGNED_BYTE indices, triangle strip and fan, normalized
# UNSIGNED_SHORT texture coordinates, a sparse accessor, TRS and matrix nodes.
blob = bytearray()
def add(data):
    while len(blob) % 4:
        blob.append(0)
    off = len(blob)
    blob.extend(data)
    return off
quad = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
o_inter = add(b"".join(struct.pack("<6f", *p, 0, 0, 1) for p in quad))
o_strip = add(bytes([0, 1, 3, 2]))
o_fan = add(bytes([0, 1, 2, 3]))
o_uv = add(struct.pack("<8H", 0, 0, 65535, 0, 65535, 65535, 0, 65535))
o_sidx = add(struct.pack("<H", 2))
o_sval = add(struct.pack("<3f", 2, 2, 0))
views = [G.BufferView(buffer=0, byteOffset=o_inter, byteLength=96, byteStride=24, target=34962),
         G.BufferView(buffer=0, byteOffset=o_strip, byteLength=4, target=34963),
         G.BufferView(buffer=0, byteOffset=o_fan, byteLength=4, target=34963),
         G.BufferView(buffer=0, byteOffset=o_uv, byteLength=16, target=34962),
         G.BufferView(buffer=0, byteOffset=o_sidx, byteLength=2),
         G.BufferView(buffer=0, byteOffset=o_sval, byteLength=12)]
acc = [G.Accessor(bufferView=0, byteOffset=0, componentType=5126, count=4, type="VEC3", min=[0, 0, 0], max=[1, 1, 0]),
       G.Accessor(bufferView=0, byteOffset=12, componentType=5126, count=4, type="VEC3"),
       G.Accessor(bufferView=1, componentType=5121, count=4, type="SCALAR"),
       G.Accessor(bufferView=2, componentType=5121, count=4, type="SCALAR"),
       G.Accessor(bufferView=3, componentType=5123, normalized=True, count=4, type="VEC2"),
       G.Accessor(bufferView=0, byteOffset=0, componentType=5126, count=4, type="VEC3", min=[0, 0, 0], max=[2, 2, 0],
                  sparse=G.Sparse(count=1, indices=G.AccessorSparseIndices(bufferView=4, componentType=5123),
                                  values=G.AccessorSparseValues(bufferView=5)))]
meshes = [G.Mesh(name="Strip", primitives=[G.Primitive(mode=5, indices=2, attributes=G.Attributes(POSITION=0, NORMAL=1, TEXCOORD_0=4))]),
          G.Mesh(name="Fan", primitives=[G.Primitive(mode=6, indices=3, attributes=G.Attributes(POSITION=0))]),
          G.Mesh(name="Sparse", primitives=[G.Primitive(mode=6, indices=3, attributes=G.Attributes(POSITION=5))])]
nodes = [G.Node(name="StripNode", mesh=0),
         G.Node(name="FanNode", mesh=1, translation=[5, 0, 0], rotation=[0, 0, 0.70710678, 0.70710678], scale=[2, 2, 2]),
         G.Node(name="Parent", matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 10, 0, 1], children=[3]),
         G.Node(name="SparseNode", mesh=2, translation=[0, 0, 1])]
g = G.GLTF2(asset=G.Asset(version="2.0", generator="pygltflib"), scene=0, scenes=[G.Scene(nodes=[0, 1, 2])], nodes=nodes,
            meshes=meshes, accessors=acc, bufferViews=views, buffers=[G.Buffer(byteLength=len(blob))])
g.set_binary_blob(bytes(blob))
g.convert_buffers(G.BufferFormat.DATAURI)
g.save("gltf/edge_cases_pygltflib.gltf")

# trimesh GLB of the sphere with vertex colours (COLOR_0 as normalized bytes).
sp = S.uv_sphere()
tm = trimesh.Trimesh(vertices=np.array(sp["positions"]), faces=np.array(S.triangulate_fan(sp["faces"])), process=False)
tm.visual.vertex_colors = np.tile(np.array([[255, 128, 0, 255]], dtype=np.uint8), (len(sp["positions"]), 1))
tm.export("gltf/sphere_trimesh_colors.glb")

# assimp: glTF 2 binary from Blender's two-object OBJ.
subprocess.run(["assimp", "export", "obj/two_objects_blender.obj", "/tmp/two_assimp.glb", "-fglb2", "-tri"], check=True,
               stdout=subprocess.DEVNULL)
shutil.move("/tmp/two_assimp.glb", "gltf/two_objects_assimp.glb")

# --- 3MF -------------------------------------------------------------------
subprocess.run(["assimp", "export", "obj/two_objects_blender.obj", "/tmp/two.3mf", "-f3mf", "-tri"], check=True,
               stdout=subprocess.DEVNULL)
shutil.move("/tmp/two.3mf", "3mf/two_objects_assimp.3mf")
# trimesh scene: the two objects with their transforms (3MF build items).
scene = trimesh.Scene()
cube = S.unit_cube()
scene.add_geometry(trimesh.Trimesh(vertices=np.array(cube["positions"]), faces=np.array(S.triangulate_fan(cube["faces"])),
                                   process=False), node_name="Box", geom_name="Box",
                   transform=trimesh.transformations.translation_matrix([2, 0, 0]))
ball = trimesh.Trimesh(vertices=np.array(sp["positions"]), faces=np.array(S.triangulate_fan(sp["faces"])), process=False)
scene.add_geometry(ball, node_name="Ball", geom_name="Ball",
                   transform=trimesh.transformations.translation_matrix([-2, 0, 0]) @ np.diag([0.5, 0.5, 0.5, 1.0]))
with open("3mf/two_objects_trimesh.3mf", "wb") as f:
    f.write(scene.export(file_type="3mf"))

# Hand-made per the 3MF core spec with Python's zipfile: units, a component
# object reusing a mesh twice with transforms, a build transform, base
# materials and a colour group (materials extension).
def model_xml():
    cube_vertices = "".join(f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in
                            [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0), (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10)])
    tris = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    red = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in tris[:6])
    blue = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}" pid="1" p1="1"/>' for a, b, c in tris[6:])
    colored = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}" pid="2" p1="0" p2="1" p3="2"/>' for a, b, c in tris)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
            'xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02">'
            '<metadata name="Title">Synthetic cubes</metadata>'
            '<resources>'
            '<basematerials id="1"><base name="Red" displaycolor="#FF0000"/><base name="Blue" displaycolor="#0000FFFF"/></basematerials>'
            '<m:colorgroup id="2"><m:color color="#FF0000"/><m:color color="#00FF00"/><m:color color="#0000FF"/></m:colorgroup>'
            f'<object id="3" type="model" name="Cube" pid="1" pindex="0"><mesh><vertices>{cube_vertices}</vertices><triangles>{red}{blue}</triangles></mesh></object>'
            f'<object id="4" type="model" name="Painted"><mesh><vertices>{cube_vertices}</vertices><triangles>{colored}</triangles></mesh></object>'
            '<object id="5" type="model" name="Pair"><components>'
            '<component objectid="3"/>'
            '<component objectid="3" transform="1 0 0 0 1 0 0 0 1 20 0 0"/>'
            '</components></object>'
            '</resources><build>'
            '<item objectid="5" transform="1 0 0 0 1 0 0 0 1 0 0 5"/>'
            '<item objectid="4" transform="0 1 0 -1 0 0 0 0 1 -20 0 0"/>'
            '</build></model>\n')
with zipfile.ZipFile("3mf/components_materials_hand.3mf", "w", zipfile.ZIP_DEFLATED) as z:
    def put(name, data):
        z.writestr(zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0)), data, zipfile.ZIP_DEFLATED)
    put("[Content_Types].xml", '<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>\n')
    put("_rels/.rels", '<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>\n')
    put("3D/3dmodel.model", model_xml())

# --- AMF -------------------------------------------------------------------
# No exporter for AMF is available in the container; written by hand per
# ISO/ASTM 52915 (AMF 1.1) and checked with assimp's AMF importer.
def amf_xml():
    out = ['<?xml version="1.0" encoding="UTF-8"?>\n<amf unit="millimeter" version="1.1">',
           '<metadata type="name">Synthetic AMF</metadata>',
           '<material id="1"><metadata type="name">Red</metadata><color><r>1</r><g>0</g><b>0</b><a>1</a></color></material>',
           '<object id="0"><metadata type="name">Cube</metadata><mesh><vertices>']
    for i, (x, y, z) in enumerate(cube["positions"]):
        c = ((i & 1) * 1.0, ((i >> 1) & 1) * 1.0, ((i >> 2) & 1) * 1.0)
        out.append(f'<vertex><coordinates><x>{x}</x><y>{y}</y><z>{z}</z></coordinates>'
                   f'<color><r>{c[0]}</r><g>{c[1]}</g><b>{c[2]}</b></color></vertex>')
    out.append('</vertices><volume materialid="1">')
    for a, b, c in S.triangulate_fan(cube["faces"]):
        out.append(f'<triangle><v1>{a}</v1><v2>{b}</v2><v3>{c}</v3></triangle>')
    out.append('</volume></mesh></object>')
    out.append('<constellation id="10"><instance objectid="0"><deltax>3</deltax><deltay>0</deltay><deltaz>0</deltaz>'
               '<rz>90</rz></instance><instance objectid="0"><deltax>-3</deltax></instance></constellation></amf>\n')
    return "\n".join(out)
with open("amf/cubes_hand.amf", "w") as f:
    f.write(amf_xml())
with zipfile.ZipFile("amf/cubes_hand_zipped.amf", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(zipfile.ZipInfo("cubes.amf", date_time=(1980, 1, 1, 0, 0, 0)), amf_xml(), zipfile.ZIP_DEFLATED)
for name in ("amf/cubes_hand.amf", "3mf/components_materials_hand.3mf", "gltf/edge_cases_pygltflib.gltf"):
    out = subprocess.run(["assimp", "info", name], capture_output=True, text=True)
    print(name, [l.strip() for l in out.stdout.splitlines() if "Vertices" in l or "Faces" in l or "Meshes" in l])
print("done")
