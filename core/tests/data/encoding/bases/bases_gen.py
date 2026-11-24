"""Writes the fixtures of core/tests/data/encoding/bases/ with independent implementations.

Input: sample.bin, 300 bytes, byte i = (i * 7 + 3) % 256 (written here too, used as reference).
"""
import base64
import email.base64mime
import email.header
import os
import ssl
import subprocess

import base45
import base58
import base91
import bech32

os.makedirs("/w/bases", exist_ok=True)
os.chdir("/w/bases")
data = bytes((i * 7 + 3) % 256 for i in range(300))
open("sample.bin", "wb").write(data)
open("sample.b45", "w").write(base45.b45encode(data).decode())
open("sample.b91", "w").write(base91.encode(data))
short = data[:32]
open("sample32.b58c", "w").write(base58.b58encode_check(short).decode())
open("sample32.bech32", "w").write(bech32.bech32_encode("test", bech32.convertbits(list(short), 8, 5)))
open("sample.mime.b64", "w", newline="").write(email.base64mime.body_encode(data, maxlinelen=76, eol="\r\n"))
open("sample.pem", "w").write(ssl.DER_cert_to_PEM_cert(data))
# RFC 2047: B and Q encoded words of a non-ASCII text, and a mixed header.
text = "Grüße aus Zürich – naïve café ☕ " * 3
open("words_b.txt", "w").write(email.header.Header(text, "utf-8", maxlinelen=76).encode())
q = email.charset.Charset("utf-8")
q.header_encoding = email.charset.QP
open("words_q.txt", "w").write(email.header.Header(text, q, maxlinelen=76).encode())
open("words_text.txt", "w").write(text)
mixed = "Subject: =?ISO-8859-1?Q?Caf=E9?= and =?UTF-8?B?w6l0w6k=?= =?UTF-8?Q?_ok?= plain"
open("words_mixed.txt", "w").write(mixed)
decoded = "".join(
    (part.decode(enc) if enc else part.decode()) if isinstance(part, bytes) else part
    for part, enc in email.header.decode_header(mixed))
open("words_mixed_decoded.txt", "w").write(decoded)
# xxencode with uudeview, BinHex with macutils.
subprocess.run(["uuenview", "-x", "-o", "sample.bin"], check=False)
xx = subprocess.run(["uuenview", "-x", "sample.bin"], capture_output=True).stdout
open("sample.xxe", "wb").write(xx)
subprocess.run(["binhex", "-d", "-t", "BINA", "-c", "CVRT", "sample.bin"], stdout=open("sample.hqx", "wb"), check=True)
print("ok")
