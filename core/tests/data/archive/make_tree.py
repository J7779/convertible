#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
"""Build the synthetic file tree that every archive fixture is made from.

    python3 make_tree.py <empty-directory>

The content is mirrored by core/tests/families/archive/tree_fixture.hpp,
which the tests compare extracted archives against. Every file and directory
gets the same fixed modification time; nothing depends on the machine.
"""
import os
import sys

MTIME = 1623758400  # 2021-06-15T12:00:00Z (an even second, so DOS times are exact)

LOREM = ("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore "
         "et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
         "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum "
         "dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
         "officia deserunt mollit anim id est laborum.\n")


def noise(count, seed=2463534242):
    out = bytearray()
    state = seed
    for _ in range(count):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        out.append(state & 0xFF)
    return bytes(out)


LONG_NAME = "long_file_name_" + "x" * 105 + ".txt"  # 124 bytes: needs GNU/pax names in tar

FILES = [
    ("hello.txt", b"Hello, archive!\n", 0o644),
    ("lorem.txt", (LOREM * 8).encode(), 0o644),
    ("noise.bin", noise(4096), 0o644),
    ("empty.txt", b"", 0o644),
    ("exec.sh", b"#!/bin/sh\necho hello\n", 0o755),
    ("dir/nested/deep/file.txt", b"deep file\n", 0o644),
    ("unicode/ünïcödé-名前.txt", b"unicode name\n", 0o644),
    ("long/" + LONG_NAME, b"long name\n", 0o644),
]
DIRS = ["dir", "dir/nested", "dir/nested/deep", "dir/empty_dir", "unicode", "long"]
LINKS = [("link", "hello.txt")]


def main():
    root = sys.argv[1]
    for d in DIRS:
        os.makedirs(os.path.join(root, d), exist_ok=True)
    for rel, data, mode in FILES:
        path = os.path.join(root, rel)
        with open(path, "wb") as f:
            f.write(data)
        os.chmod(path, mode)
        os.utime(path, (MTIME, MTIME))
    for rel, target in LINKS:
        path = os.path.join(root, rel)
        os.symlink(target, path)
        os.utime(path, (MTIME, MTIME), follow_symlinks=False)
    for d in sorted(DIRS, key=len, reverse=True):
        path = os.path.join(root, d)
        os.chmod(path, 0o755)
        os.utime(path, (MTIME, MTIME))
    os.chmod(root, 0o755)


if __name__ == "__main__":
    main()
