# Throwaway image with the independent tools that wrote the mesh fixtures.
# Only used to (re)generate fixtures and to check Convertible's output; the
# product never runs any of these programs.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends blender assimp-utils gmsh python3-venv python3-pip \
      python3-numpy ca-certificates curl xz-utils libgl1 libegl1 libgomp1 libxi6 libxxf86vm1 libxfixes3 libxrender1 \
      libxkbcommon0 libsm6 libice6 libx11-6 libxext6 && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /venv && /venv/bin/pip install --no-cache-dir numpy trimesh plyfile pygltflib laspy meshio numpy-stl \
      usd-core pypcd4 py-vox-io lxml networkx pymeshlab open3d
# Blender 4.1.1 from blender.org: unlike the distribution package it includes
# the COLLADA and USD exporters.
RUN curl -fsSL https://download.blender.org/release/Blender4.1/blender-4.1.1-linux-x64.tar.xz -o /tmp/b.tar.xz && \
    mkdir -p /opt && tar -xJf /tmp/b.tar.xz -C /opt && rm /tmp/b.tar.xz && \
    ln -s /opt/blender-4.1.1-linux-x64/blender /usr/local/bin/blender41
