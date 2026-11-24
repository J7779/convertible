# Sources: encoding/hashes

Reference digests for `core/tests/families/encoding/hash_test.cpp`, computed by
independent implementations. The inputs are not stored: the input of length `n`
is the byte sequence `i -> (i * 31 + 7) % 251` for `i = 0 .. n-1`, which the test
regenerates.

| File | Contents | Tools | Command |
| --- | --- | --- | --- |
| `vectors.txt` | 1080 lines `<algorithm>[:variant] <length> <hex digest>` for 27 lengths (0 .. 4096 and 10000); digests are cut to 32 hex digits except for lengths 0 and 1000 | Ubuntu 24.04 container: Python 3.12.3 `hashlib`/`zlib`/`binascii`, pycryptodome (MD2, Keccak, RIPEMD-160), `mmh3` 5.3.1, OpenSSL 3.0.13 (`dgst -sm3`, HMAC), RHash 1.4.3 (MD4, Whirlpool), `b3sum` 1.2.0, `xxhsum` 0.8.2 | `docker run --rm -v "$PWD":/w <image with xxhash b3sum rhash python3> sh -c 'pip3 install pycryptodome mmh3 && cd /w && python3 hash_vectors.py'`, then digests shortened with `python3 -c` as described above |
| `hash_vectors.py` | the script that wrote `vectors.txt` (kept for regeneration; not used by the build or the tests) | — | — |

Variants: `shake128:100` = 100-byte output; `blake2b:key=000102..3f,len=32` = key bytes 0..63 and a
32-byte digest; `blake2s:key=000102..1f` = key bytes 0..31; `blake3:len=100` = `b3sum --length 100`;
`murmur3:seed=42`; `sha256:hmac=key`, `md5:hmac=key` = `openssl dgst -hmac key`; `sha3-512:hmac=long` =
HMAC with the 106-character key written in the script.

Other expected values in the test come from the specifications themselves: RFC 1319/1320 (MD2/MD4 test
suites), FIPS 180-4 and FIPS 202 examples, RFC 7693 (BLAKE2), the BLAKE3 test vectors, the RIPEMD-160,
Whirlpool and SM3 reference examples, the SipHash paper, RFC 2202/4231 (HMAC), the CRC catalogue check
values, and Subresource Integrity values checked with
`printf abc | openssl dgst -sha384 -binary | openssl base64 -A` (OpenSSL 4.1 on macOS).
