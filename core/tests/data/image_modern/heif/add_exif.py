# Inserts a synthetic EXIF APP1 segment (Orientation, 72 dpi, Software "TestPattern")
# right after the SOI marker of a JPEG. Standard library only.
# Usage: python3 add_exif.py <in.jpg> <out.jpg> <orientation 1..8>
import struct
import sys


def main():
    src, dst, orientation = sys.argv[1], sys.argv[2], int(sys.argv[3])
    with open(src, "rb") as f:
        data = f.read()
    assert data[:2] == b"\xff\xd8", "not a JPEG file"
    count = 5
    data_offset = 8 + 2 + count * 12 + 4
    extra = b""

    def add(blob):
        nonlocal extra
        offset = data_offset + len(extra)
        extra += blob
        return offset

    software = b"TestPattern\x00"
    xres = add(struct.pack(">II", 72, 1))
    yres = add(struct.pack(">II", 72, 1))
    soft = add(software)
    entries = [
        struct.pack(">HHI", 0x0112, 3, 1) + struct.pack(">HH", orientation, 0),
        struct.pack(">HHII", 0x011A, 5, 1, xres),
        struct.pack(">HHII", 0x011B, 5, 1, yres),
        struct.pack(">HHI", 0x0128, 3, 1) + struct.pack(">HH", 2, 0),
        struct.pack(">HHII", 0x0131, 2, len(software), soft),
    ]
    tiff = b"MM\x00*" + struct.pack(">IH", 8, count) + b"".join(entries) + struct.pack(">I", 0) + extra
    app1 = b"Exif\x00\x00" + tiff
    segment = b"\xff\xe1" + struct.pack(">H", len(app1) + 2) + app1
    with open(dst, "wb") as f:
        f.write(data[:2] + segment + data[2:])


if __name__ == "__main__":
    main()
