FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends gdal-bin python3-gdal gpsbabel osmium-tool \
    python3-shapely python3-pyproj python3-fiona python3-pyshp python3-venv python3-pip python3-numpy python3-protobuf \
    nodejs npm ca-certificates && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv --system-site-packages /venv && /venv/bin/pip install --no-cache-dir fitparse fit-tool topojson geobuf polyline openlocationcode pygeohash
RUN mkdir /npm && cd /npm && npm init -y >/dev/null && npm install --no-audit --no-fund topojson-server@3.0.1 topojson-client@3.1.0 flatgeobuf@3.36.0
WORKDIR /w
