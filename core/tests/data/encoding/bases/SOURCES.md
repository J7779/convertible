# Sources: encoding/bases

Fixtures for `core/tests/families/encoding/more_bases_test.cpp`, written by independent
implementations in a throwaway Ubuntu 24.04 container (Python 3.12.3). `bases_gen.py` (kept here,
not used by the build or the tests) wrote all of them:

```sh
docker run --rm -v "$PWD/..":/w <ubuntu:24.04 + python3, uudeview, macutils> sh -c \
  'pip3 install base45==0.4.4 base58==2.1.1 base91==1.0.1 bech32==1.2.0 && cd /w && python3 bases/bases_gen.py'
```

| File | Contents | Tool |
| --- | --- | --- |
| `sample.bin` | 300 bytes, byte i = (i * 7 + 3) % 256 | Python |
| `sample.b45` | Base45 of `sample.bin` | `base45.b45encode` (base45 0.4.4) |
| `sample.b91` | basE91 of `sample.bin` | `base91.encode` (base91 1.0.1) |
| `sample32.b58c` | Base58Check of the first 32 bytes | `base58.b58encode_check` (base58 2.1.1) |
| `sample32.bech32` | Bech32, prefix `test`, of the first 32 bytes | `bech32.bech32_encode("test", convertbits(data, 8, 5))` (bech32 1.2.0, the BIP 173 reference code) |
| `sample.mime.b64` | MIME Base64, 76 columns, CRLF | `email.base64mime.body_encode(data, maxlinelen=76, eol="\r\n")` |
| `sample.pem` | PEM armour, label CERTIFICATE | `ssl.DER_cert_to_PEM_cert(data)` |
| `sample.xxe` | xxencoded `sample.bin` | `uuenview -x sample.bin` (uudeview 0.5pl20) |
| `sample.hqx` | BinHex 4.0, type `BINA`, creator `CVRT` | `binhex -d -t BINA -c CVRT sample.bin` (macutils 2.0b3) |
| `words_text.txt` | a short made-up German/French phrase with an emoji, repeated 3 times | Python |
| `words_b.txt`, `words_q.txt` | RFC 2047 B and Q encoded words of `words_text.txt` | `email.header.Header(text, charset, maxlinelen=76).encode()` |
| `words_mixed.txt`, `words_mixed_decoded.txt` | a header mixing ISO-8859-1 Q, UTF-8 B and Q words with plain text, and its decoding | `email.header.decode_header` |

Interop of our own output (checked while developing, same container): `base45.b45decode`,
`base91.decode`, `base58.b58decode_check`, `bech32.bech32_decode` + `convertbits`,
`ssl.PEM_cert_to_DER_cert`, `base64.b64decode` and `email.header.decode_header` all return the original
data for what our encoders write; `hexbin -d` (macutils) restores `sample.bin` from our `.hqx`; our
xxencode output is line-for-line identical to `uuenview -x` (which adds one trailing blank line).
Base92 and Base122 have no packaged reference implementation available offline; they are covered by
hand-computed vectors and exhaustive round trips only.
