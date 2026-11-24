"""Extract the first H.264 video track of an MP4/MOV into a tiny packet file.

Layout (all integers big-endian):
  8 bytes  magic "CVTPKT1\\0"
  u32      timescale
  u32      avcC length, then the avcC payload
  u32      packet count
  per packet: u32 size, i64 pts, i64 dts, u8 keyframe, payload (length-prefixed NAL units as stored)
"""
import struct
import sys


def boxes(data, start, end):
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        header = 8
        if size == 1:
            size = struct.unpack(">Q", data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = end - pos
        yield kind.decode("latin-1"), pos + header, pos + size
        pos += size


def find(data, start, end, path):
    for kind, body, stop in boxes(data, start, end):
        if kind == path[0]:
            if len(path) == 1:
                return body, stop
            found = find(data, body, stop, path[1:])
            if found:
                return found
    return None


def main(src, dst):
    data = open(src, "rb").read()
    moov = find(data, 0, len(data), ["moov"])
    for kind, body, stop in boxes(data, moov[0], moov[1]):
        if kind != "trak":
            continue
        hdlr = find(data, body, stop, ["mdia", "hdlr"])
        if data[hdlr[0] + 8:hdlr[0] + 12] != b"vide":
            continue
        mdhd = find(data, body, stop, ["mdia", "mdhd"])
        version = data[mdhd[0]]
        timescale = struct.unpack(">I", data[mdhd[0] + (20 if version == 1 else 12):][:4])[0]
        stbl = find(data, body, stop, ["mdia", "minf", "stbl"])
        stsd = find(data, stbl[0], stbl[1], ["stsd"])
        entry = stsd[0] + 8
        esize = struct.unpack(">I", data[entry:entry + 4])[0]
        avcc = find(data, entry + 8 + 78, entry + esize, ["avcC"])
        avcc_bytes = data[avcc[0]:avcc[1]]

        def table(name):
            return find(data, stbl[0], stbl[1], [name])

        b, _ = table("stsz")
        fixed, count = struct.unpack(">II", data[b + 4:b + 12])
        sizes = [fixed] * count if fixed else list(struct.unpack(">%dI" % count, data[b + 12:b + 12 + 4 * count]))
        co = table("stco")
        if co:
            n = struct.unpack(">I", data[co[0] + 4:co[0] + 8])[0]
            offsets = list(struct.unpack(">%dI" % n, data[co[0] + 8:co[0] + 8 + 4 * n]))
        else:
            co = table("co64")
            n = struct.unpack(">I", data[co[0] + 4:co[0] + 8])[0]
            offsets = list(struct.unpack(">%dQ" % n, data[co[0] + 8:co[0] + 8 + 8 * n]))
        b, _ = table("stsc")
        n = struct.unpack(">I", data[b + 4:b + 8])[0]
        stsc = [struct.unpack(">III", data[b + 8 + 12 * i:b + 20 + 12 * i]) for i in range(n)]
        positions = []
        sample = 0
        for chunk in range(len(offsets)):
            per = 0
            for first, spc, _ in stsc:
                if first - 1 <= chunk:
                    per = spc
            pos = offsets[chunk]
            for _ in range(per):
                if sample < count:
                    positions.append(pos)
                    pos += sizes[sample]
                    sample += 1
        b, _ = table("stts")
        n = struct.unpack(">I", data[b + 4:b + 8])[0]
        dts = []
        t = 0
        for i in range(n):
            c, d = struct.unpack(">II", data[b + 8 + 8 * i:b + 16 + 8 * i])
            for _ in range(c):
                dts.append(t)
                t += d
        offsets_ct = [0] * count
        ct = table("ctts")
        if ct:
            n = struct.unpack(">I", data[ct[0] + 4:ct[0] + 8])[0]
            k = 0
            for i in range(n):
                c, o = struct.unpack(">Ii", data[ct[0] + 8 + 8 * i:ct[0] + 16 + 8 * i])
                for _ in range(c):
                    offsets_ct[k] = o
                    k += 1
        keys = None
        ss = table("stss")
        if ss:
            n = struct.unpack(">I", data[ss[0] + 4:ss[0] + 8])[0]
            keys = set(struct.unpack(">%dI" % n, data[ss[0] + 8:ss[0] + 8 + 4 * n]))
        out = bytearray(b"CVTPKT1\0")
        out += struct.pack(">II", timescale, len(avcc_bytes)) + avcc_bytes
        out += struct.pack(">I", count)
        for i in range(count):
            payload = data[positions[i]:positions[i] + sizes[i]]
            key = 1 if keys is None or (i + 1) in keys else 0
            out += struct.pack(">IqqB", len(payload), dts[i] + offsets_ct[i], dts[i], key) + payload
        open(dst, "wb").write(out)
        return
    raise SystemExit("no video track")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
