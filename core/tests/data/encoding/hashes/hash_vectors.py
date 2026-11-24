"""Reference digests for core/tests/data/encoding/hashes/vectors.txt.

Run inside the throwaway container (python3 + pip packages + CLI tools).
Input of length n: byte i = (i * 31 + 7) % 251.
Output lines: "<algorithm> <length> <hex>" (and "<algorithm>:<variant> ..." for parametrised forms).
"""
import hashlib
import subprocess
import zlib
import binascii
import tempfile
import os

from Crypto.Hash import MD2, keccak, RIPEMD160
import mmh3

LENGTHS = [0, 1, 3, 4, 8, 9, 16, 17, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 200, 240, 241, 1000, 1024, 1025,
           3000, 4096, 10000]


def data(n):
    return bytes((i * 31 + 7) % 251 for i in range(n))


out = []


def emit(name, n, digest_hex):
    out.append("%s %d %s" % (name, n, digest_hex.lower()))


def tool(args, payload):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(payload)
        path = f.name
    try:
        res = subprocess.run(args + [path], capture_output=True, check=True).stdout.decode()
    finally:
        os.unlink(path)
    return res


for n in LENGTHS:
    d = data(n)
    for name in ["md5", "sha1", "sha224", "sha256", "sha384", "sha512", "sha512_224", "sha512_256", "sha3_224",
                 "sha3_256", "sha3_384", "sha3_512", "blake2b", "blake2s"]:
        emit(name.replace("_", "-") if name.startswith("sha512_") else name.replace("_", "-"), n,
             hashlib.new(name, d).hexdigest())
    emit("shake128", n, hashlib.shake_128(d).hexdigest(32))
    emit("shake256", n, hashlib.shake_256(d).hexdigest(64))
    emit("shake128:100", n, hashlib.shake_128(d).hexdigest(100))
    emit("blake2b:key=000102..3f,len=32", n, hashlib.blake2b(d, key=bytes(range(64)), digest_size=32).hexdigest())
    emit("blake2s:key=000102..1f", n, hashlib.blake2s(d, key=bytes(range(32))).hexdigest())
    emit("md2", n, MD2.new(d).hexdigest())
    emit("keccak256", n, keccak.new(data=d, digest_bits=256).hexdigest())
    emit("keccak512", n, keccak.new(data=d, digest_bits=512).hexdigest())
    emit("ripemd160", n, RIPEMD160.new(d).hexdigest())
    emit("crc32", n, "%08x" % zlib.crc32(d))
    emit("adler32", n, "%08x" % zlib.adler32(d))
    emit("crc16-xmodem", n, "%04x" % binascii.crc_hqx(d, 0))
    emit("murmur3", n, "%08x" % mmh3.hash(d, 0, signed=False))
    emit("murmur3:seed=42", n, "%08x" % mmh3.hash(d, 42, signed=False))
    h1, h2 = mmh3.hash64(d, 0, signed=False)
    emit("murmur3-128", n, "%016x%016x" % (h1, h2))
    # CLI tools
    emit("blake3", n, tool(["b3sum", "--no-names"], d).split()[0])
    emit("blake3:len=100", n, tool(["b3sum", "--no-names", "--length", "100"], d).split()[0])
    emit("xxh32", n, tool(["xxhsum", "-H0"], d).split()[0])
    emit("xxh64", n, tool(["xxhsum", "-H1"], d).split()[0])
    xxh3 = tool(["xxhsum", "-H3"], d).split()
    emit("xxh3", n, xxh3[-1] if "=" in xxh3 else xxh3[0])
    xxh3 = tool(["xxhsum", "-H3", "--tag"], d).split()
    out.append("# xxh3 tag line: " + " ".join(xxh3[:1])) if n == 0 else None
    emit("md4", n, tool(["rhash", "--md4", "--simple"], d).split()[0])
    emit("whirlpool", n, tool(["rhash", "--whirlpool", "--simple"], d).split()[0])
    sm3 = tool(["openssl", "dgst", "-sm3", "-r"], d).split()[0]
    emit("sm3", n, sm3)
    hmac = tool(["openssl", "dgst", "-sha256", "-hmac", "key", "-r"], d).split()[0]
    emit("sha256:hmac=key", n, hmac)
    hmac = tool(["openssl", "dgst", "-sha3-512", "-hmac", "a much longer key than the block of sha3 would allow, really much longer than one hundred bytes......", "-r"], d).split()[0]
    emit("sha3-512:hmac=long", n, hmac)
    hmac = tool(["openssl", "dgst", "-md5", "-hmac", "key", "-r"], d).split()[0]
    emit("md5:hmac=key", n, hmac)

open("/w/vectors.txt", "w").write("\n".join(out) + "\n")
print(len(out), "lines")
