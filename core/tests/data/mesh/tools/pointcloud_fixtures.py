# Point cloud (XYZ/PTS/PCD/LAS), VTK and Gmsh fixtures (run from tests/data/mesh):
#   /venv/bin/python tools/pointcloud_fixtures.py
import os
import shutil
import subprocess
import sys

import laspy
import meshio
import numpy as np
import open3d as o3d

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenes_np as S  # noqa: E402

for d in ("xyz", "pcd", "las", "vtk", "msh"):
    os.makedirs(d, exist_ok=True)

pts, cols = S.point_grid()
P = np.array(pts, dtype=np.float64)
C = np.array(cols, dtype=np.float64) / 255.0
N = np.tile(np.array([[0.0, 0.0, 1.0]]), (len(pts), 1))

# --- Open3D: PCD in all three encodings and the XYZ family ---------------------
cloud = o3d.geometry.PointCloud()
cloud.points = o3d.utility.Vector3dVector(P)
cloud.colors = o3d.utility.Vector3dVector(C)
cloud.normals = o3d.utility.Vector3dVector(N)
o3d.io.write_point_cloud("pcd/points_open3d_ascii.pcd", cloud, write_ascii=True)
o3d.io.write_point_cloud("pcd/points_open3d_binary.pcd", cloud, write_ascii=False, compressed=False)
o3d.io.write_point_cloud("pcd/points_open3d_compressed.pcd", cloud, write_ascii=False, compressed=True)
o3d.io.write_point_cloud("xyz/points_open3d.xyz", cloud)
o3d.io.write_point_cloud("xyz/points_open3d.xyzn", cloud)
o3d.io.write_point_cloud("xyz/points_open3d.xyzrgb", cloud)
o3d.io.write_point_cloud("xyz/points_open3d.pts", cloud)

# --- laspy: LAS 1.2 (format 3: GPS time + RGB) and 1.4 (format 7 + extra bytes) --
for version, fmt, name in (("1.2", 3, "las/points_laspy_v12_f3.las"), ("1.4", 7, "las/points_laspy_v14_f7.las")):
    header = laspy.LasHeader(point_format=fmt, version=version)
    header.scales = np.array([0.001, 0.001, 0.001])
    header.offsets = np.array([1000.0, 2000.0, 0.0])
    if fmt == 7:
        header.add_extra_dim(laspy.ExtraBytesParams(name="quality", type=np.float32))
    las = laspy.LasData(header)
    las.x = P[:, 0] + 1000.0
    las.y = P[:, 1] + 2000.0
    las.z = P[:, 2]
    las.intensity = np.arange(len(pts), dtype=np.uint16) * 10
    las.classification = np.full(len(pts), 2, dtype=np.uint8)
    las.gps_time = np.arange(len(pts), dtype=np.float64) * 0.5
    las.red = np.array([c[0] for c in cols], dtype=np.uint16) * 257
    las.green = np.array([c[1] for c in cols], dtype=np.uint16) * 257
    las.blue = np.array([c[2] for c in cols], dtype=np.uint16) * 257
    if fmt == 7:
        las.quality = np.linspace(0, 1, len(pts)).astype(np.float32)
    las.write(name)

# --- meshio: VTK legacy 4.2 / 5.1, ASCII / binary --------------------------------
cube, sphere = S.unit_cube(), S.uv_sphere()
quads = np.array(cube["faces"])
meshio.write("vtk/cube_meshio_42_ascii.vtk", meshio.Mesh(np.array(cube["positions"]), [("quad", quads)],
             point_data={"height": np.array(cube["positions"])[:, 1]}), file_format="vtk42", binary=False)
tris = np.array(S.triangulate_fan(sphere["faces"]))
meshio.write("vtk/sphere_meshio_51_binary.vtk", meshio.Mesh(np.array(sphere["positions"]), [("triangle", tris)]),
             file_format="vtk", binary=True)
meshio.write("vtk/sphere_meshio_42_binary.vtk", meshio.Mesh(np.array(sphere["positions"]), [("triangle", tris)]),
             file_format="vtk42", binary=True)

# --- Gmsh: a 2 x 1 plate (area 2), triangles and recombined quads ---------------
geo = """// 2 x 1 plate
SetFactory("Built-in");
Point(1) = {0, 0, 0, 0.5};
Point(2) = {2, 0, 0, 0.5};
Point(3) = {2, 1, 0, 0.5};
Point(4) = {0, 1, 0, 0.5};
Line(1) = {1, 2};
Line(2) = {2, 3};
Line(3) = {3, 4};
Line(4) = {4, 1};
Curve Loop(1) = {1, 2, 3, 4};
Plane Surface(1) = {1};
Physical Surface("Plate") = {1};
%s
"""
for name, extra, args in (("msh/plate_gmsh22_ascii.msh", "", ["-format", "msh22"]),
                          ("msh/plate_gmsh22_binary.msh", "", ["-format", "msh22", "-bin"]),
                          ("msh/plate_gmsh41_ascii.msh", "", ["-format", "msh41"]),
                          ("msh/plate_quads_gmsh41.msh", "Recombine Surface{1};", ["-format", "msh41"])):
    with open("/tmp/plate.geo", "w") as f:
        f.write(geo % extra)
    subprocess.run(["gmsh", "/tmp/plate.geo", "-2", "-o", "/tmp/out.msh"] + args, check=True, stdout=subprocess.DEVNULL)
    shutil.copy("/tmp/out.msh", name)
print("done")
