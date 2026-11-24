# OFF fixtures: MeshLab (pymeshlab) and trimesh writing the canonical scenes.
import os, sys
import numpy as np, pymeshlab, trimesh
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_np as S
out = "off/"  # run from tests/data/mesh
os.makedirs(out, exist_ok=True)
c = S.unit_cube()
ms = pymeshlab.MeshSet()
cols = np.array([[(i & 1) * 1.0, ((i >> 1) & 1) * 1.0, ((i >> 2) & 1) * 1.0, 1.0] for i in range(8)])
ms.add_mesh(pymeshlab.Mesh(vertex_matrix=np.array(c["positions"], dtype=float),
                           face_matrix=np.array(S.triangulate_fan(c["faces"])), v_color_matrix=cols))
ms.save_current_mesh(out + "cube_meshlab.off", save_vertex_color=True)
s = S.uv_sphere()
trimesh.Trimesh(vertices=np.array(s["positions"]), faces=np.array(S.triangulate_fan(s["faces"])),
                process=False).export(out + "sphere_trimesh.off")
