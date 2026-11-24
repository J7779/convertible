# Removes machine details the exporters write into fixtures (run last, from
# tests/data/mesh): temporary directory paths, author placeholders and
# export dates. Binary files are edited with same-length replacements so
# every length field stays valid.
#   python3 tools/scrub_fixtures.py
import os
import re

TMP = b"/tmp/cvt-blender/"
SAME_LENGTH = b"textures/blender/"
assert len(TMP) == len(SAME_LENGTH)

for root, _, files in os.walk("."):
    if root.startswith("./tools"):
        continue
    for name in files:
        path = os.path.join(root, name)
        with open(path, "rb") as f:
            data = f.read()
        original = data
        data = data.replace(TMP, SAME_LENGTH)
        # The container mounts this directory at /w: "/w/fbx/..." -> "../fbx/..." (same length).
        data = re.sub(rb"/w/(fbx|dae|x3d|usd|3ds|gltf|obj|ply|stl|x)/", rb"../\1/", data)
        if name.endswith((".dae", ".x3d", ".usda", ".amf", ".wrl")):
            data = re.sub(rb"<author>[^<]*</author>", b"", data)
            data = re.sub(rb"<created>[^<]*</created>", b"<created>2000-01-01T00:00:00</created>", data)
            data = re.sub(rb"<modified>[^<]*</modified>", b"<modified>2000-01-01T00:00:00</modified>", data)
            data = re.sub(rb"content='[0-9:\- ]+' name='(created|modified)'", rb"content='2000-01-01' name='\1'", data)
            data = re.sub(rb'content="[^"]*" name="filename"', b'content="model" name="filename"', data)
        if data != original:
            with open(path, "wb") as f:
                f.write(data)
            print("scrubbed", path)
