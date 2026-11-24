# codec_vpx: libvpx (VP8 and VP9 decoders and encoders) as a generic, C-only
# library. The configuration headers in core/src/imported/libvpx/config/ were
# produced once by the upstream configure for a generic target (no assembly, no
# SIMD, no threads, VP9 high bit depth enabled) and made host-neutral; nothing
# is configured or generated at build time.
cvt_add_imported(libvpx
  C_STANDARD 11
  INCLUDE_DIRS . config
  SOURCES
    config/vpx_config.c
    vp8/common/alloccommon.c
    vp8/common/blockd.c
    vp8/common/dequantize.c
    vp8/common/entropy.c
    vp8/common/entropymode.c
    vp8/common/entropymv.c
    vp8/common/extend.c
    vp8/common/filter.c
    vp8/common/findnearmv.c
    vp8/common/generic/systemdependent.c
    vp8/common/idct_blk.c
    vp8/common/idctllm.c
    vp8/common/loopfilter_filters.c
    vp8/common/mbpitch.c
    vp8/common/modecont.c
    vp8/common/quant_common.c
    vp8/common/reconinter.c
    vp8/common/reconintra.c
    vp8/common/reconintra4x4.c
    vp8/common/rtcd.c
    vp8/common/setupintrarecon.c
    vp8/common/swapyv12buffer.c
    vp8/common/treecoder.c
    vp8/common/vp8_loopfilter.c
    vp8/common/vp8_skin_detection.c
    vp8/decoder/dboolhuff.c
    vp8/decoder/decodeframe.c
    vp8/decoder/decodemv.c
    vp8/decoder/detokenize.c
    vp8/decoder/onyxd_if.c
    vp8/encoder/bitstream.c
    vp8/encoder/boolhuff.c
    vp8/encoder/copy_c.c
    vp8/encoder/dct.c
    vp8/encoder/denoising.c
    vp8/encoder/encodeframe.c
    vp8/encoder/encodeintra.c
    vp8/encoder/encodemb.c
    vp8/encoder/encodemv.c
    vp8/encoder/firstpass.c
    vp8/encoder/lookahead.c
    vp8/encoder/mcomp.c
    vp8/encoder/modecosts.c
    vp8/encoder/onyx_if.c
    vp8/encoder/pickinter.c
    vp8/encoder/picklpf.c
    vp8/encoder/ratectrl.c
    vp8/encoder/rdopt.c
    vp8/encoder/segmentation.c
    vp8/encoder/temporal_filter.c
    vp8/encoder/tokenize.c
    vp8/encoder/treewriter.c
    vp8/encoder/vp8_quantize.c
    vp8/vp8_cx_iface.c
    vp8/vp8_dx_iface.c
    vp9/common/vp9_alloccommon.c
    vp9/common/vp9_blockd.c
    vp9/common/vp9_common_data.c
    vp9/common/vp9_entropy.c
    vp9/common/vp9_entropymode.c
    vp9/common/vp9_entropymv.c
    vp9/common/vp9_filter.c
    vp9/common/vp9_frame_buffers.c
    vp9/common/vp9_idct.c
    vp9/common/vp9_loopfilter.c
    vp9/common/vp9_mvref_common.c
    vp9/common/vp9_pred_common.c
    vp9/common/vp9_quant_common.c
    vp9/common/vp9_reconinter.c
    vp9/common/vp9_reconintra.c
    vp9/common/vp9_rtcd.c
    vp9/common/vp9_scale.c
    vp9/common/vp9_scan.c
    vp9/common/vp9_seg_common.c
    vp9/common/vp9_thread_common.c
    vp9/common/vp9_tile_common.c
    vp9/decoder/vp9_decodeframe.c
    vp9/decoder/vp9_decodemv.c
    vp9/decoder/vp9_decoder.c
    vp9/decoder/vp9_detokenize.c
    vp9/decoder/vp9_dsubexp.c
    vp9/decoder/vp9_job_queue.c
    vp9/encoder/vp9_alt_ref_aq.c
    vp9/encoder/vp9_aq_360.c
    vp9/encoder/vp9_aq_complexity.c
    vp9/encoder/vp9_aq_cyclicrefresh.c
    vp9/encoder/vp9_aq_variance.c
    vp9/encoder/vp9_bitstream.c
    vp9/encoder/vp9_context_tree.c
    vp9/encoder/vp9_cost.c
    vp9/encoder/vp9_dct.c
    vp9/encoder/vp9_encodeframe.c
    vp9/encoder/vp9_encodemb.c
    vp9/encoder/vp9_encodemv.c
    vp9/encoder/vp9_encoder.c
    vp9/encoder/vp9_ethread.c
    vp9/encoder/vp9_ext_ratectrl.c
    vp9/encoder/vp9_extend.c
    vp9/encoder/vp9_firstpass.c
    vp9/encoder/vp9_frame_scale.c
    vp9/encoder/vp9_lookahead.c
    vp9/encoder/vp9_mbgraph.c
    vp9/encoder/vp9_mcomp.c
    vp9/encoder/vp9_multi_thread.c
    vp9/encoder/vp9_noise_estimate.c
    vp9/encoder/vp9_picklpf.c
    vp9/encoder/vp9_pickmode.c
    vp9/encoder/vp9_quantize.c
    vp9/encoder/vp9_ratectrl.c
    vp9/encoder/vp9_rd.c
    vp9/encoder/vp9_rdopt.c
    vp9/encoder/vp9_resize.c
    vp9/encoder/vp9_segmentation.c
    vp9/encoder/vp9_skin_detection.c
    vp9/encoder/vp9_speed_features.c
    vp9/encoder/vp9_subexp.c
    vp9/encoder/vp9_svc_layercontext.c
    vp9/encoder/vp9_temporal_filter.c
    vp9/encoder/vp9_tokenize.c
    vp9/encoder/vp9_tpl_model.c
    vp9/encoder/vp9_treewriter.c
    vp9/vp9_cx_iface.c
    vp9/vp9_dx_iface.c
    vp9/vp9_iface_common.c
    vpx/src/vpx_codec.c
    vpx/src/vpx_decoder.c
    vpx/src/vpx_encoder.c
    vpx/src/vpx_image.c
    vpx_dsp/avg.c
    vpx_dsp/bitreader.c
    vpx_dsp/bitreader_buffer.c
    vpx_dsp/bitwriter.c
    vpx_dsp/bitwriter_buffer.c
    vpx_dsp/fwd_txfm.c
    vpx_dsp/intrapred.c
    vpx_dsp/inv_txfm.c
    vpx_dsp/loopfilter.c
    vpx_dsp/prob.c
    vpx_dsp/psnr.c
    vpx_dsp/quantize.c
    vpx_dsp/sad.c
    vpx_dsp/skin_detection.c
    vpx_dsp/sse.c
    vpx_dsp/subtract.c
    vpx_dsp/sum_squares.c
    vpx_dsp/variance.c
    vpx_dsp/vpx_convolve.c
    vpx_dsp/vpx_dsp_rtcd.c
    vpx_mem/vpx_mem.c
    vpx_scale/generic/gen_scalers.c
    vpx_scale/generic/vpx_scale.c
    vpx_scale/generic/yv12config.c
    vpx_scale/generic/yv12extend.c
    vpx_scale/vpx_scale_rtcd.c
    vpx_util/vpx_thread.c
)
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBVPX=1)

# libvpx is far too slow to be useful unoptimised; keep it optimised in Debug
# builds too (sanitizer instrumentation still applies). MSVC is left alone:
# its Debug runtime checks (/RTC1) cannot be combined with /O2.
if(NOT MSVC)
  target_compile_options(cvt_imported_libvpx PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

# Sanitizer builds: corrupt streams make libvpx's integer transforms overflow
# (its C code assumes in-range coefficients; the wrap-around is harmless on
# every target and the decoder rejects or conceals the frame). Keep AddressSanitizer
# and every other UBSan check, but not that one, for the imported code only.
if(CVT_SANITIZE AND NOT MSVC)
  target_compile_options(cvt_imported_libvpx PRIVATE -fno-sanitize=signed-integer-overflow)
endif()
