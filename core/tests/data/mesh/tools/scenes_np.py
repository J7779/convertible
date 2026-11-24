"""Canonical synthetic scenes (Y-up), identical to core/tests/families/mesh/scenes.hpp.

Plain Python (no numpy needed) so Blender's bundled Python can import it too.
Each scene: dict(name, positions=[(x,y,z)], faces=[[i...]], normals?, uvs?, colors?)
"""
import math


def unit_cube():
    p = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5),
         (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
    f = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]]
    return dict(name="Cube", positions=p, faces=f)


def uv_sphere(segments=16, rings=8, radius=1.0):
    p = [(0.0, radius, 0.0)]
    for r in range(1, rings):
        phi = math.pi * r / rings
        for s in range(segments):
            th = 2 * math.pi * s / segments
            p.append((radius * math.sin(phi) * math.cos(th), radius * math.cos(phi), -radius * math.sin(phi) * math.sin(th)))
    p.append((0.0, -radius, 0.0))
    ring = lambda r, s: 1 + (r - 1) * segments + (s % segments)
    bottom = len(p) - 1
    f = [[0, ring(1, s), ring(1, s + 1)] for s in range(segments)]
    for r in range(1, rings - 1):
        for s in range(segments):
            f.append([ring(r, s), ring(r + 1, s), ring(r + 1, s + 1), ring(r, s + 1)])
    f += [[bottom, ring(rings - 1, s + 1), ring(rings - 1, s)] for s in range(segments)]
    return dict(name="Sphere", positions=p, faces=f)


def concave_star():
    p = []
    for k in range(10):
        a = math.pi / 2 + k * math.pi / 5
        r = 1.0 if k % 2 == 0 else 0.4
        p.append((r * math.cos(a), r * math.sin(a), 0.0))
    return dict(name="Star", positions=p, faces=[list(range(10))])


def textured_quad():
    return dict(name="Quad", positions=[(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], faces=[[0, 1, 2, 3]],
                normals=[(0, 0, 1)] * 4, uvs=[(0, 0), (1, 0), (1, 1), (0, 1)],
                colors=[(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1), (1, 1, 1, 1)], material="Checker")


def transformed(scene, scale=1.0, translate=(0, 0, 0), name=None):
    out = dict(scene)
    out["positions"] = [(x * scale + translate[0], y * scale + translate[1], z * scale + translate[2]) for x, y, z in scene["positions"]]
    if name:
        out["name"] = name
    return out


def two_objects():
    """[(name, local scene, scale, translation)] - Box at (2,0,0), Ball scaled 0.5 at (-2,0,0)."""
    return [("Box", unit_cube(), 1.0, (2.0, 0.0, 0.0)), ("Ball", uv_sphere(), 0.5, (-2.0, 0.0, 0.0))]


def point_grid():
    pts, cols = [], []
    for y in range(10):
        for x in range(10):
            fx, fy = 0.1 * x, 0.1 * y
            pts.append((fx, fy, 0.1 * (fx + fy)))
            cols.append((round(255 * x / 9), round(255 * y / 9), 128))
    return pts, cols


def triangulate_fan(faces):
    out = []
    for f in faces:
        for k in range(1, len(f) - 1):
            out.append([f[0], f[k], f[k + 1]])
    return out


def star_triangles():
    """A correct triangulation of the concave star (fan from an outer tip is wrong)."""
    # Inner pentagon 1,3,5,7,9 as a fan, plus the five tips.
    tris = [[(k + 9) % 10, k, (k + 1) % 10] for k in range(0, 10, 2)]
    inner = [1, 3, 5, 7, 9]
    tris += [[inner[0], inner[i], inner[i + 1]] for i in range(1, 4)]
    return tris


def to_z_up(p):
    """Y-up (x, y, z) -> Z-up (x, -z, y), how Blender stores a Y-up model."""
    x, y, z = p
    return (x, -z, y)


# 4x4 RGBA checker PNG (black / white), made with zlib from the stdlib.
def checker_png():
    import struct, zlib
    w = h = 4
    raw = b""
    for y in range(h):
        raw += b"\x00"
        for x in range(w):
            v = 255 if (x + y) % 2 == 0 else 0
            raw += bytes((v, v, v, 255))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
