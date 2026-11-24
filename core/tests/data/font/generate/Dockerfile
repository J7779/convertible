FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8 LANG=C.UTF-8 TZ=UTC SOURCE_DATE_EPOCH=0
RUN apt-get update && apt-get install -y --no-install-recommends \
      python3 python3-venv python3-pip python3-fontforge fontforge-nox \
      woff2 t1utils xfonts-utils eot-utils bdf2psf console-setup-linux \
      binutils file ca-certificates build-essential meson ninja-build pkg-config git \
    && rm -rf /var/lib/apt/lists/*
# opentype-sanitizer has no aarch64 wheel; its sdist build breaks with setuptools>=72
# (copy_file(dry_run=...) removed), so pin setuptools for the isolated build env.
RUN python3 -m venv --system-site-packages /opt/venv \
    && echo "setuptools<72" > /tmp/c.txt \
    && PIP_CONSTRAINT=/tmp/c.txt /opt/venv/bin/pip install --no-cache-dir opentype-sanitizer \
    && /opt/venv/bin/pip install --no-cache-dir "fonttools[woff]" Pillow numpy \
    && ln -s "$(/opt/venv/bin/python3 -c 'import ots,os;print(os.path.dirname(ots.__file__))')/ots-sanitize" /usr/local/bin/ots-sanitize
ENV PATH=/opt/venv/bin:$PATH
WORKDIR /w
