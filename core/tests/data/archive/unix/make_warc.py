#!/usr/bin/env python3
# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
"""Write small WARC files following the WARC 1.0 / 1.1 specification (ISO 28500).

    python3 make_warc.py <lorem.txt> <out-dir>

Produces demo.warc (WARC/1.0, uncompressed) and demo.warc.gz (the same records
as WARC/1.1, one gzip member per record). Everything is synthetic: example.com /
example.org URIs, a fixed date, fixed record ids.
"""
import gzip
import hashlib
import base64
import io
import os
import sys

DATE = "2021-06-15T12:00:00Z"


def record(version, rtype, n, headers, block):
    lines = [f"WARC/{version}", f"WARC-Type: {rtype}", f"WARC-Date: {DATE}",
             f"WARC-Record-ID: <urn:uuid:00000000-0000-4000-8000-{n:012d}>"]
    lines += headers
    digest = base64.b32encode(hashlib.sha1(block).digest()).decode()
    lines.append(f"WARC-Block-Digest: sha1:{digest}")
    lines.append(f"Content-Length: {len(block)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + block + b"\r\n\r\n"


def http(status, ctype, body, extra=()):
    head = [f"HTTP/1.1 {status}", f"Content-Type: {ctype}"] + list(extra)
    if not any(h.startswith("Transfer-Encoding") for h in extra):
        head.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(head) + "\r\n\r\n").encode() + body


def chunked(body, size):
    out = b""
    for i in range(0, len(body), size):
        part = body[i:i + size]
        out += f"{len(part):x}\r\n".encode() + part + b"\r\n"
    return out + b"0\r\n\r\n"


def records(version, lorem):
    uri = (lambda u: f"<{u}>") if version == "1.0" else (lambda u: u)
    index = b"<!doctype html><title>Demo</title><p>Hello, archive!</p>\n"
    page = b"<!doctype html><title>Page</title><p>A page.</p>\n"
    out = []
    out.append(record(version, "warcinfo", 1, ["Content-Type: application/warc-fields"],
                      b"software: make_warc.py\r\nformat: WARC File Format\r\n"))
    out.append(record(version, "request", 2, [f"WARC-Target-URI: {uri('http://example.com/')}",
                                              "Content-Type: application/http; msgtype=request"],
                      b"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"))
    out.append(record(version, "response", 3, [f"WARC-Target-URI: {uri('http://example.com/')}",
                                               "Content-Type: application/http; msgtype=response"],
                      http("200 OK", "text/html", index)))
    out.append(record(version, "response", 4, [f"WARC-Target-URI: {uri('https://example.com/docs/page.html?lang=en&x=1')}",
                                               "Content-Type: application/http; msgtype=response"],
                      http("200 OK", "text/html", page)))
    # The same URI again (a re-fetch): de-duplicated as page~2.html.
    out.append(record(version, "response", 5, [f"WARC-Target-URI: {uri('https://example.com/docs/page.html?lang=en&x=1')}",
                                               "Content-Type: application/http; msgtype=response"],
                      http("200 OK", "text/html", page.replace(b"A page", b"A newer page"))))
    # Chunked transfer coding.
    out.append(record(version, "response", 6, [f"WARC-Target-URI: {uri('http://example.com/lorem.txt')}",
                                               "Content-Type: application/http; msgtype=response"],
                      http("200 OK", "text/plain", chunked(lorem, 1000), ["Transfer-Encoding: chunked"])))
    # A gzip content coding.
    gz = gzip.compress(b"Hello, archive!\n", mtime=0)
    out.append(record(version, "response", 7, [f"WARC-Target-URI: {uri('http://example.org/hello.txt')}",
                                               "Content-Type: application/http; msgtype=response"],
                      http("200 OK", "text/plain", gz, ["Content-Encoding: gzip"])))
    # A resource record (no HTTP headers) for a directory-like URI.
    out.append(record(version, "resource", 8, [f"WARC-Target-URI: {uri('http://example.org/a/b/')}",
                                               "Content-Type: text/plain"],
                      b"resource body\n"))
    # Metadata and revisit records carry no files.
    out.append(record(version, "metadata", 9, [f"WARC-Target-URI: {uri('http://example.com/')}",
                                               "Content-Type: application/warc-fields"],
                      b"via: http://example.com/\r\n"))
    out.append(record(version, "revisit", 10, [f"WARC-Target-URI: {uri('http://example.com/')}",
                                               "WARC-Profile: http://netpreserve.org/warc/1.0/revisit/identical-payload-digest",
                                               "Content-Type: application/http; msgtype=response"],
                      b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"))
    return out


def main():
    lorem = open(sys.argv[1], "rb").read()
    out_dir = sys.argv[2]
    with open(os.path.join(out_dir, "demo.warc"), "wb") as f:
        f.write(b"".join(records("1.0", lorem)))
    with open(os.path.join(out_dir, "demo.warc.gz"), "wb") as f:
        for r in records("1.1", lorem):
            buf = io.BytesIO()
            with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0, compresslevel=9, filename="") as g:
                g.write(r)
            f.write(buf.getvalue())


if __name__ == "__main__":
    main()
