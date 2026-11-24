FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends libreoffice-calc-nogui gnumeric python3 python3-venv python3-pip sqlite3 && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /venv && /venv/bin/pip install --no-cache-dir pyarrow pandas xlwt xlrd openpyxl odfpy pyreadstat
WORKDIR /w
