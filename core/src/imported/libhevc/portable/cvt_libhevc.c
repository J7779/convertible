/*
 * Narrow C interface to the libhevc decoder (see api/cvt_libhevc.h).
 * Written for Convertible; Apache-2.0 like the code it wraps.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ihevc_defs.h"
#include "ihevcd_cxa.h"

#include "cvt_libhevc.h"

struct cvt_libhevc_decoder {
  iv_obj_t *handle;
};

/* Aligned allocation without platform calls: the original pointer is stored
 * just before the aligned block. */
static void *cvt_aligned_alloc(void *ctxt, WORD32 alignment, WORD32 size) {
  (void)ctxt;
  if(size <= 0 || alignment <= 0 || alignment > 4096 || (alignment & (alignment - 1)) != 0) {
    if(size <= 0) return NULL;
    alignment = 16;
  }
  if(alignment < (WORD32)sizeof(void *)) alignment = (WORD32)sizeof(void *);
  {
    const size_t total = (size_t)size + (size_t)alignment + sizeof(void *);
    uint8_t *raw = (uint8_t *)malloc(total);
    uintptr_t start;
    uint8_t *aligned;
    if(!raw) return NULL;
    start = (uintptr_t)(raw + sizeof(void *));
    start = (start + (uintptr_t)alignment - 1u) & ~((uintptr_t)alignment - 1u);
    aligned = (uint8_t *)start;
    memcpy(aligned - sizeof(void *), &raw, sizeof(void *));
    return aligned;
  }
}

static void cvt_aligned_free(void *ctxt, void *buf) {
  uint8_t *raw;
  (void)ctxt;
  if(!buf) return;
  memcpy(&raw, (uint8_t *)buf - sizeof(void *), sizeof(void *));
  free(raw);
}

static IV_API_CALL_STATUS_T cvt_call(iv_obj_t *handle, void *ip, void *op) {
  return ihevcd_cxa_api_function(handle, ip, op);
}

static int cvt_set_params(iv_obj_t *handle) {
  ivd_ctl_set_config_ip_t ip;
  ivd_ctl_set_config_op_t op;
  memset(&ip, 0, sizeof(ip));
  memset(&op, 0, sizeof(op));
  ip.u4_size = sizeof(ip);
  op.u4_size = sizeof(op);
  ip.e_cmd = IVD_CMD_VIDEO_CTL;
  ip.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
  ip.e_vid_dec_mode = IVD_DECODE_FRAME;
  ip.u4_disp_wd = 0;
  ip.e_frm_skip_mode = IVD_SKIP_NONE;
  ip.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
  return cvt_call(handle, &ip, &op) == IV_SUCCESS ? 0 : -1;
}

static int cvt_set_one_core(iv_obj_t *handle) {
  ihevcd_cxa_ctl_set_num_cores_ip_t ip;
  ihevcd_cxa_ctl_set_num_cores_op_t op;
  memset(&ip, 0, sizeof(ip));
  memset(&op, 0, sizeof(op));
  ip.u4_size = sizeof(ip);
  op.u4_size = sizeof(op);
  ip.e_cmd = IVD_CMD_VIDEO_CTL;
  ip.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IHEVCD_CXA_CMD_CTL_SET_NUM_CORES;
  ip.u4_num_cores = 1;
  return cvt_call(handle, &ip, &op) == IV_SUCCESS ? 0 : -1;
}

cvt_libhevc_decoder *cvt_libhevc_create(int out_layout) {
  ihevcd_cxa_create_ip_t ip;
  ihevcd_cxa_create_op_t op;
  cvt_libhevc_decoder *decoder;
  iv_obj_t *handle;
  IV_COLOR_FORMAT_T format = IV_YUV_420P;
  if(out_layout == CVT_LIBHEVC_OUT_422) format = IV_YUV_422P;
  if(out_layout == CVT_LIBHEVC_OUT_444) format = IV_YUV_444P;

  decoder = (cvt_libhevc_decoder *)calloc(1, sizeof(*decoder));
  if(!decoder) return NULL;

  memset(&ip, 0, sizeof(ip));
  memset(&op, 0, sizeof(op));
  ip.s_ivd_create_ip_t.u4_size = sizeof(ip);
  op.s_ivd_create_op_t.u4_size = sizeof(op);
  ip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
  ip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
  ip.s_ivd_create_ip_t.e_output_format = format;
  ip.s_ivd_create_ip_t.pf_aligned_alloc = cvt_aligned_alloc;
  ip.s_ivd_create_ip_t.pf_aligned_free = cvt_aligned_free;
  ip.s_ivd_create_ip_t.pv_mem_ctxt = NULL;
  ip.u4_keep_threads_active = 0;
  ip.u4_enable_yuv_formats = (1u << CHROMA_FMT_IDC_MONOCHROME) | (1u << CHROMA_FMT_IDC_YUV420) |
                             (1u << CHROMA_FMT_IDC_YUV422) | (1u << CHROMA_FMT_IDC_YUV444);
  ip.u4_enable_hbd = 1;
  ip.u4_thumbnail_mode = 0;

  if(cvt_call(NULL, &ip, &op) != IV_SUCCESS || !op.s_ivd_create_op_t.pv_handle) {
    if(op.s_ivd_create_op_t.pv_handle) {
      ivd_delete_ip_t dip;
      ivd_delete_op_t dop;
      handle = (iv_obj_t *)op.s_ivd_create_op_t.pv_handle;
      handle->pv_fxns = (void *)ihevcd_cxa_api_function;
      handle->u4_size = sizeof(iv_obj_t);
      memset(&dip, 0, sizeof(dip));
      memset(&dop, 0, sizeof(dop));
      dip.u4_size = sizeof(dip);
      dop.u4_size = sizeof(dop);
      dip.e_cmd = IVD_CMD_DELETE;
      cvt_call(handle, &dip, &dop);
    }
    free(decoder);
    return NULL;
  }
  handle = (iv_obj_t *)op.s_ivd_create_op_t.pv_handle;
  handle->pv_fxns = (void *)ihevcd_cxa_api_function;
  handle->u4_size = sizeof(iv_obj_t);
  decoder->handle = handle;

  if(cvt_set_one_core(handle) != 0 || cvt_set_params(handle) != 0) {
    cvt_libhevc_destroy(decoder);
    return NULL;
  }
  return decoder;
}

void cvt_libhevc_destroy(cvt_libhevc_decoder *decoder) {
  if(!decoder) return;
  if(decoder->handle) {
    ivd_delete_ip_t ip;
    ivd_delete_op_t op;
    memset(&ip, 0, sizeof(ip));
    memset(&op, 0, sizeof(op));
    ip.u4_size = sizeof(ip);
    op.u4_size = sizeof(op);
    ip.e_cmd = IVD_CMD_DELETE;
    cvt_call(decoder->handle, &ip, &op);
  }
  free(decoder);
}

int cvt_libhevc_decode(cvt_libhevc_decoder *decoder, const uint8_t *data, uint32_t size,
                       uint32_t timestamp, cvt_libhevc_io *io) {
  ivd_video_decode_ip_t ip;
  ivd_video_decode_op_t op;
  IV_API_CALL_STATUS_T status;
  int i;

  memset(&ip, 0, sizeof(ip));
  memset(&op, 0, sizeof(op));
  ip.u4_size = sizeof(ip);
  op.u4_size = sizeof(op);
  ip.e_cmd = IVD_CMD_VIDEO_DECODE;
  ip.u4_ts = timestamp;
  ip.pv_stream_buffer = (void *)data;
  ip.u4_num_Bytes = size;
  ip.s_out_buffer.u4_num_bufs = 3;
  for(i = 0; i < 3; i++) {
    ip.s_out_buffer.pu1_bufs[i] = io->planes[i];
    ip.s_out_buffer.u4_min_out_buf_size[i] = io->plane_sizes[i];
  }

  status = cvt_call(decoder->handle, &ip, &op);

  io->bytes_consumed = op.u4_num_bytes_consumed;
  io->picture_decoded = op.u4_frame_decoded_flag ? 1 : 0;
  io->output_present = op.u4_output_present ? 1 : 0;
  io->output_timestamp = op.u4_ts;
  io->width = op.u4_pic_wd;
  io->height = op.u4_pic_ht;
  io->bit_depth = op.u4_bit_depth;
  io->error_code = op.u4_error_code;
  io->fatal = ((op.u4_error_code >> IVD_FATALERROR) & 1u) ? 1 : 0;
  return status == IV_SUCCESS ? 0 : -1;
}

int cvt_libhevc_flush(cvt_libhevc_decoder *decoder) {
  ivd_ctl_flush_ip_t ip;
  ivd_ctl_flush_op_t op;
  memset(&ip, 0, sizeof(ip));
  memset(&op, 0, sizeof(op));
  ip.u4_size = sizeof(ip);
  op.u4_size = sizeof(op);
  ip.e_cmd = IVD_CMD_VIDEO_CTL;
  ip.e_sub_cmd = IVD_CMD_CTL_FLUSH;
  return cvt_call(decoder->handle, &ip, &op) == IV_SUCCESS ? 0 : -1;
}

int cvt_libhevc_reset(cvt_libhevc_decoder *decoder) {
  ivd_ctl_reset_ip_t ip;
  ivd_ctl_reset_op_t op;
  memset(&ip, 0, sizeof(ip));
  memset(&op, 0, sizeof(op));
  ip.u4_size = sizeof(ip);
  op.u4_size = sizeof(op);
  ip.e_cmd = IVD_CMD_VIDEO_CTL;
  ip.e_sub_cmd = IVD_CMD_CTL_RESET;
  if(cvt_call(decoder->handle, &ip, &op) != IV_SUCCESS) return -1;
  return cvt_set_params(decoder->handle);
}
