// Convertible core - codec_legacy family: notes for container (muxer and
// demuxer) authors. Documentation only; nothing here is compiled.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
//
// Look codecs up with `ctx.registry->media().make_video_decoder(id)` (and
// `make_video_encoder`). Ids that codec/ids.hpp does not list yet are only
// reachable through `legacy::make_video_decoder(id)` in codec_legacy.hpp
// until they are appended there (they are then registered automatically):
// flv1 huffyuv ffv1 utvideo cinepak msrle msvideo1 qtrle rpza smc uyvy yuy2 v210 bgr24 r210.
// Not implemented: dnxhd, dvvideo, msmpeg4v3 (ids reserved in codec_legacy.hpp).
//
// General conventions
//   * One packet = one coded picture unless noted. `Packet::pts` is carried
//     to the frame; `INT64_MIN` means "no timestamp" (MPEG-PS/TS PES packets
//     without a PTS). For AVI, give every packet pts = INT64_MIN and
//     dts = the chunk index (in `timescale` units): decoders with
//     reordering then derive presentation times from the dts sequence.
//   * Empty packets (AVI "dropped frame" chunks) repeat the previous picture
//     for the intra/delta codecs below (not for MPEG/H.263, which ignore them).
//   * `CodecParams::width/height` are required for the codecs whose
//     bitstream does not carry the size: uyvy, yuy2, v210, bgr24, r210,
//     huffyuv, ffv1 (versions 0/1), utvideo, msrle, msvideo1, qtrle, rpza, smc.
//   * `CodecParams::bits_per_sample` carries the coded depth where the
//     container stores it outside the bitstream: BITMAPINFOHEADER.biBitCount
//     (AVI) or the QuickTime sample description depth (MOV; 33/34/36/40 are
//     the grey 1/2/4/8-bit variants).
//   * Palettes: extradata = up to 256 entries of B, G, R, 0 (the AVI RGBQUAD
//     layout that follows the BITMAPINFOHEADER). Convert a MOV 'stsd' colour
//     table (16-bit A, R, G, B per entry) to that layout.
//   * Decoders deliver their native layout (I420/I422/I444, the P10 variants,
//     RGBA8) with ColorInfo set; encoders accept I420 and their native
//     layouts (anything else is converted with codec/colorspace.hpp).
//
// mpeg1video / mpeg2video
//   MP4 'mp4v' with ObjectTypeIndication 0x6A (MPEG-1) / 0x60-0x65 (MPEG-2),
//   MKV V_MPEG1 / V_MPEG2, AVI 'mpg1'/'mpg2'/'MPEG', MPEG-PS/TS streams 0x01/0x02.
//   Packets may be whole pictures or arbitrary elementary-stream pieces (PES
//   payloads); the decoder reassembles at start codes. Extradata (optional):
//   sequence header + extensions. The encoder repeats the sequence header and
//   a GOP header before every I picture and returns the sequence header (and
//   sequence extension) as extradata; packets carry pts and dts (B-pictures
//   reorder; dts lags by one frame).
//
// mpeg4 (MPEG-4 Part 2)
//   AVI 'XVID','DIVX','DX50','FMP4','MP4V' (DIVX 3.x 'DIV3' is msmpeg4v3: not
//   supported), MP4 'mp4v' (OTI 0x20), MKV V_MPEG4/ISO/ASP or /SP.
//   Extradata: the VOS/VO/VOL headers (MP4 esds DecoderSpecificInfo, MKV
//   CodecPrivate) or empty when they are in band. DivX/Xvid "packed
//   bitstreams" (a P- and a B-VOP in one AVI chunk followed by a not-coded
//   VOP) are handled; pass every chunk unchanged. The encoder writes VOS/VO/VOL
//   both as extradata and in band before every I-VOP; no B-VOPs (pts == dts).
//   Not decoded: GMC with 2 or 3 warping points (Xvid "GMC"), static sprites,
//   shapes, RVLC partitions (concealed).
//
// h263 / flv1
//   MP4/3GP 's263' (and 'h263'), AVI 'H263','U263', MKV V_MS/VFW/FOURCC.
//   No extradata. The encoder uses baseline headers for 128x96, 176x144,
//   352x288, 704x576, 1408x1152 and PLUSPTYPE custom formats otherwise (sizes
//   multiple of 4, up to 2048x1152; such streams need an H.263v2 decoder).
//   FLV: codec id 2 (Sorenson H.263) = flv1; packets are the video tag bodies
//   without the 1-byte FLV video header. Disposable frames (type 2) are decoded
//   and never used as references.
//
// theora
//   MKV V_THEORA CodecPrivate (Xiph lacing), MP4/FFmpeg style (16-bit length
//   prefixed packets) or Ogg (the three header packets as the first packets,
//   recognised by their 0x80/0x81/0x82 first byte). The encoder returns the
//   headers Xiph-laced. Granule positions are the container's business.
//
// prores
//   MOV/MP4 'apco' (Proxy), 'apcs' (LT), 'apcn' (standard), 'apch' (HQ),
//   'ap4h' (4444), 'ap4x' (4444 XQ); MKV V_PRORES with the fourcc as private
//   data. No extradata. Packets are whole frames ('icpf'). The encoder writes
//   4:2:2 (choose apch for quality >= 90, apcn >= 75, apcs >= 55, apco below)
//   or, when configured with PixelFormat::RGBA8, 4:4:4 with alpha ('ap4h').
//
// huffyuv / ffv1 / utvideo
//   AVI 'HFYU' (extradata = BITMAPINFOHEADER tail with the code tables;
//   bits_per_sample 16/24/32), 'FFV1' (versions 0/1: no extradata, version 3:
//   the configuration record), MKV V_MS/VFW/FOURCC or V_FFV1; Ut Video 'ULY0',
//   'ULY2','ULY4','ULH0','ULH2','ULH4','ULRG','ULRA' with the 16-byte codec
//   private data as extradata (pass bits_per_sample 12/16/48/24/32 to name the
//   layout). Encoders: HuffYUV 4:2:2 (median) or RGBA (configure RGBA8);
//   FFV1 version 3 with CRCs in I420/I422/I444/P10/RGBA8 layouts (extradata =
//   configuration record). All lossless; every packet is a key frame unless an
//   FFV1 keyframe_interval > 1 is requested.
//
// qtrle / rpza / smc / cinepak / msrle / msvideo1
//   MOV 'rle ', 'rpza', 'smc ', 'cvid'; AVI 'cvid', 'mrle' (or BI_RLE8 = 1,
//   BI_RLE4 = 2 compression), 'CRAM'/'MSVC'/'WHAM'. Delta codecs: the decoder
//   keeps the previous picture; start decoding at a key frame. AVI bottom-up
//   orientation is handled by the decoders. The qtrle encoder writes 24-bit
//   (default) or 32-bit ARGB when configured with PixelFormat::RGBA8: set the
//   sample description depth accordingly; key frames every keyframe_interval
//   (default 60), "no change" frames are 4-byte packets.
//
// uyvy / yuy2 / v210 / bgr24 / r210
//   MOV '2vuy'/'UYVY' -> uyvy, 'yuvs'/'YUY2' -> yuy2, 'v210', 'r210';
//   AVI BI_RGB 24-bit -> bgr24 (bottom-up; for a negative biHeight pass
//   extradata "top-down"). rawvideo itself is not registered by this family.
#pragma once
