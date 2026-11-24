#!/usr/bin/env python3
# Convertible test fixtures: packet dumps and reference frames for the
# codec_legacy tests. Runs inside the fixture container next to ffmpeg/ffprobe.
#
#   fixture_tool.py pkt <media file> <out.pkt> <codec id> [--xiph] [--divx-packed]
#       Writes every packet of the first video stream, with timestamps and
#       the stream extradata, in the CVTPKT01 format read by
#       tests/families/codec_legacy/legacy_test_support.hpp.
#   fixture_tool.py ref <raw file> <out.ref> <pix_fmt> <width> <height> [--md5] [--frames=0,5,9]
#       Stores decoded frames (ffmpeg -f rawvideo output, presentation order)
#       in the CVTREF01 format: zlib-compressed planes, or with --md5 only an
#       MD5 per frame (for bit-exact codecs). --frames keeps only those frames.
#   fixture_tool.py mkv <in.pkt> <out.mkv> <codec spec>
#       Wraps a packet dump (e.g. our encoder output) in a minimal Matroska
#       file for decoding with independent software (see cmd_mkv).
#
# CVTPKT01 (little-endian): magic, u8 n + codec id, u32 width, u32 height,
# u32 timescale, u32 n + extradata, u32 packet count, then per packet
# u32 size, i64 pts, i64 dts (INT64_MIN = unknown), u8 flags (1 = key), data.
# CVTPKTZ1: the same with everything after the magic zlib-compressed.
# CVTREF01: magic, u8 n + pix_fmt, u32 width, u32 height, u32 total frames,
# u32 stored frames, u32 index per stored frame, then 'Z' + zlib(planes of
# the stored frames, ffmpeg rawvideo layout) or 'M' + 16-byte MD5 per stored frame.
import hashlib
import json
import struct
import subprocess
import sys
import zlib

NONE = -(1 << 63)


def hexdump_to_bytes(text):
    out = bytearray()
    for line in text.splitlines():
        if ': ' not in line:
            continue
        hexpart = line.split(': ', 1)[1][:40]
        out += bytes.fromhex(hexpart.replace(' ', ''))
    return bytes(out)


def ffprobe(path, *args):
    result = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-of', 'json', *args, path],
                            check=True, capture_output=True)
    return json.loads(result.stdout)


def xiph_lace(packets):
    out = bytearray([len(packets) - 1])
    for p in packets[:-1]:
        n = len(p)
        while n >= 255:
            out.append(255)
            n -= 255
        out.append(n)
    for p in packets:
        out += p
    return bytes(out)


def split_ffmpeg_xiph(data):
    # ffmpeg keeps Xiph headers as three 16-bit big-endian length-prefixed packets.
    packets, at = [], 0
    while at + 2 <= len(data) and len(packets) < 3:
        n = (data[at] << 8) | data[at + 1]
        packets.append(data[at + 2:at + 2 + n])
        at += 2 + n
    return packets


def not_coded_vop(time_bits):
    # vop_coding_type P (01), modulo_time_base 0, marker, vop_time_increment 0,
    # marker, vop_coded 0, then MPEG-4 stuffing (a 0 and 1s up to the byte end).
    bits = '01' + '0' + '1' + '0' * time_bits + '1' + '0'
    bits += '0' + '1' * ((8 - (len(bits) + 1) % 8) % 8)
    return b'\x00\x00\x01\xb6' + int(bits, 2).to_bytes(len(bits) // 8, 'big')


def divx_pack(packets, time_bits):
    # DivX "packed bitstream": a P-VOP and the B-VOP that follows it in decode
    # order share one AVI chunk; the B-VOP's own chunk then holds a not-coded
    # VOP. Emulated from an ordinary stream, the way DivX 5 wrote AVI files.
    def vop_type(data):
        at = data.find(b'\x00\x00\x01\xb6')
        return None if at < 0 or at + 4 >= len(data) else data[at + 4] >> 6

    out, i = [], 0
    while i < len(packets):
        p = dict(packets[i])
        if vop_type(p['data']) == 1 and i + 1 < len(packets) and vop_type(packets[i + 1]['data']) == 2:
            b = packets[i + 1]
            p['data'] = p['data'] + b['data']
            out.append(p)
            out.append({'data': not_coded_vop(time_bits), 'pts': b['pts'], 'dts': b['dts'], 'flags': 0})
            i += 2
            continue
        out.append(p)
        i += 1
    return out


def mov_palette(path):
    # The colour table of the first video sample description of a QuickTime
    # file, as 256 RGBQUADs (B, G, R, 0) - the convention our palettised
    # decoders use for extradata (as in AVI).
    data = open(path, 'rb').read()
    at = data.find(b'stsd')
    entry = at + 12  # box type, version/flags, entry count
    depth, ctab_id = struct.unpack('>Hh', data[entry + 82:entry + 86])
    if depth > 8 or ctab_id != 0:
        raise SystemExit('no colour table in %s' % path)
    table = entry + 86
    _seed, _flags, size = struct.unpack('>IHH', data[table:table + 8])
    quads = bytearray(1024)
    for i in range(size + 1):
        _value, r, g, b = struct.unpack(">HHHH", data[table + 8 + 8 * i:table + 16 + 8 * i])
        index = i  # entries are in order; the value field is not reliable
        if index < 256:
            quads[4 * index:4 * index + 4] = bytes([b >> 8, g >> 8, r >> 8, 0])
    return bytes(quads)


def cmd_pkt(src, dst, codec, options):
    streams = ffprobe(src, '-show_streams', '-show_data')['streams']
    stream = streams[0]
    extradata = hexdump_to_bytes(stream.get('extradata', '')) if 'extradata' in stream else b''
    if '--mov-palette' in options:
        extradata = mov_palette(src)
    if '--xiph' in options and extradata and extradata[:2] == b'\x00\x2a':
        # 16-bit length-prefixed headers -> Xiph lacing (the Matroska/MP4 form).
        extradata = xiph_lace(split_ffmpeg_xiph(extradata))
    tb_num, tb_den = (int(x) for x in stream['time_base'].split('/'))
    timescale = tb_den // tb_num if tb_den % tb_num == 0 else tb_den
    raw = ffprobe(src, '-show_packets', '-show_data')['packets']
    packets = []
    for p in raw:
        def ts(key):
            v = p.get(key)
            return NONE if v in (None, 'N/A') else int(v)
        packets.append({'data': hexdump_to_bytes(p['data']), 'pts': ts('pts'), 'dts': ts('dts'),
                        'flags': 1 if 'K' in p.get('flags', '') else 0})
    for option in options:
        if option.startswith('--divx-packed='):
            packets = divx_pack(packets, int(option.split('=')[1]))
    out = bytearray(b'CVTPKT01')
    out += bytes([len(codec)]) + codec.encode()
    out += struct.pack('<III', int(stream['width']), int(stream['height']), timescale)
    out += struct.pack('<I', len(extradata)) + extradata
    out += struct.pack('<I', len(packets))
    for p in packets:
        out += struct.pack('<Iqqb', len(p['data']), p['pts'], p['dts'], p['flags']) + p['data']
    packed = zlib.compress(bytes(out[8:]), 9)
    if len(packed) + 8 < len(out):
        out = b'CVTPKTZ1' + packed  # same content, zlib-compressed after the magic
    with open(dst, 'wb') as f:
        f.write(out)


def frame_bytes(pix_fmt, w, h):
    cw, ch2, ch4 = (w + 1) // 2, (h + 1) // 2, w
    table = {
        'yuv420p': w * h + 2 * cw * ch2, 'yuv422p': w * h + 2 * cw * h, 'yuv444p': 3 * w * h,
        'yuv411p': w * h + 2 * ((w + 3) // 4) * h, 'gray': w * h,
        'yuv420p10le': 2 * (w * h + 2 * cw * ch2), 'yuv422p10le': 2 * (w * h + 2 * cw * h),
        'yuv444p10le': 6 * w * h, 'yuva444p10le': 8 * w * h, 'gbrp10le': 6 * w * h,
        'rgba': 4 * w * h, 'rgb24': 3 * w * h, 'bgr24': 3 * w * h, 'rgb48le': 6 * w * h,
    }
    del ch4
    return table[pix_fmt]


def cmd_ref(src, dst, pix_fmt, w, h, options):
    data = open(src, 'rb').read()
    size = frame_bytes(pix_fmt, w, h)
    total = len(data) // size
    keep = None
    md5 = '--md5' in options
    for option in options:
        if option.startswith('--frames='):
            keep = [int(x) for x in option.split('=')[1].split(',')]
    indices = list(range(total)) if keep is None else [i for i in keep if i < total]
    out = bytearray(b'CVTREF01')
    out += bytes([len(pix_fmt)]) + pix_fmt.encode()
    out += struct.pack('<IIII', w, h, total, len(indices))
    out += b''.join(struct.pack('<I', i) for i in indices)
    if md5:
        out += b'M' + b''.join(hashlib.md5(data[i * size:(i + 1) * size]).digest() for i in indices)
    else:
        out += b'Z' + zlib.compress(b''.join(data[i * size:(i + 1) * size] for i in indices), 9)
    with open(dst, 'wb') as f:
        f.write(out)


def read_pkt(path):
    data = open(path, 'rb').read()
    if data[:8] == b'CVTPKTZ1':
        data = b'CVTPKT01' + zlib.decompress(data[8:])
    assert data[:8] == b'CVTPKT01'
    at = 8
    n = data[at]
    codec = data[at + 1:at + 1 + n].decode()
    at += 1 + n
    w, h, timescale, extra_len = struct.unpack_from('<IIII', data, at)
    at += 16
    extradata = data[at:at + extra_len]
    at += extra_len
    count, = struct.unpack_from('<I', data, at)
    at += 4
    packets = []
    for _ in range(count):
        size, pts, dts, flags = struct.unpack_from('<Iqqb', data, at)
        at += 21
        packets.append({'data': data[at:at + size], 'pts': pts, 'dts': dts, 'flags': flags})
        at += size
    return codec, w, h, timescale, extradata, packets


def ebml_size(n):
    length = 1
    while n >= (1 << (7 * length)) - 1:
        length += 1
    return ((1 << (7 * length)) | n).to_bytes(length, 'big')


def el(ident, payload):
    if isinstance(payload, int):
        payload = payload.to_bytes(max(1, (payload.bit_length() + 7) // 8), 'big')
    elif isinstance(payload, str):
        payload = payload.encode()
    return bytes.fromhex(ident) + ebml_size(len(payload)) + payload


def cmd_mkv(src, dst, codec_spec):
    # Wraps a CVTPKT01 dump (for instance our encoder output) in a minimal
    # Matroska file so that independent decoders can read it. codec_spec:
    #   V_MPEG1, V_MPEG2, V_MPEG4/ISO/ASP, V_THEORA, ...  codec private = extradata
    #   V_MS/VFW/FOURCC:<fourcc>[:<bits per pixel>]      BITMAPINFOHEADER + extradata
    #   V_PRORES:<fourcc>                                codec private = fourcc
    #   V_QUICKTIME:<fourcc>:<depth>                     QuickTime sample description
    _, w, h, timescale, extradata, packets = read_pkt(src)
    parts = codec_spec.split(':')
    codec_id, private = parts[0], extradata
    if codec_id == 'V_MS/VFW/FOURCC':
        bpp = int(parts[2]) if len(parts) > 2 else 24
        private = struct.pack('<IiiHH4sIiiII', 40 + len(extradata), w, h, 1, bpp, parts[1].encode(),
                              w * h * 4, 0, 0, 0, 0) + extradata
    elif codec_id == 'V_PRORES':
        private = parts[1].encode()
    elif codec_id == 'V_QUICKTIME':
        depth = int(parts[2])
        entry = (parts[1].ljust(4).encode() + b'\0' * 6 + struct.pack('>H', 1) + struct.pack('>HH4sII', 0, 0, b'\0\0\0\0', 0, 0)
                 + struct.pack('>HHIIIH', w, h, 72 << 16, 72 << 16, 0, 1) + b'\0' * 32 + struct.pack('>hh', depth, -1)
                 + extradata)
        private = struct.pack('>I', 4 + len(entry)) + entry
    video = el('B0', w) + el('BA', h)
    track = el('D7', 1) + el('73C5', 1) + el('83', 1) + el('86', codec_id) + el('E0', video)
    if private:
        track += el('63A2', private)
    header = el('1A45DFA3', el('4286', 1) + el('42F7', 1) + el('42F2', 4) + el('42F3', 8) + el('4282', 'matroska')
                + el('4287', 4) + el('4285', 2))
    body = el('1549A966', el('2AD7B1', 1000000) + el('4D80', 'fixture_tool') + el('5741', 'fixture_tool'))
    body += el('1654AE6B', el('AE', track))
    for p in packets:
        pts = p['pts'] if p['pts'] != NONE else p['dts']
        ms = pts * 1000 // timescale
        block = b'\x81' + struct.pack('>hB', 0, 0x80 if p['flags'] & 1 else 0) + p['data']
        body += el('1F43B675', el('E7', max(ms, 0)) + el('A3', block))
    with open(dst, 'wb') as f:
        f.write(header + el('18538067', body))


def main():
    if sys.argv[1] == 'pkt':
        cmd_pkt(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    elif sys.argv[1] == 'ref':
        cmd_ref(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), int(sys.argv[6]), sys.argv[7:])
    elif sys.argv[1] == 'mkv':
        cmd_mkv(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        raise SystemExit('unknown command')


if __name__ == '__main__':
    main()
