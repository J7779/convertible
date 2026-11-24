/*****************************************************************************
 * This file is part of Kvazaar HEVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ****************************************************************************/

/*
 * C-only build: stand-ins for the SIMD strategy registration functions of
 * src/strategies/{avx2,sse2,sse41,altivec}. With KVZ_C_ONLY the SIMD sources
 * would compile to exactly these empty registrations (their bodies are
 * guarded by COMPILE_INTEL_* / COMPILE_POWERPC_*), so they are not shipped.
 * The hardware flags are never set in this build, so none of these is called.
 */
#include <stdint.h>

#include "strategies/altivec/picture-altivec.h"
#include "strategies/avx2/dct-avx2.h"
#include "strategies/avx2/encode_coding_tree-avx2.h"
#include "strategies/avx2/intra-avx2.h"
#include "strategies/avx2/ipol-avx2.h"
#include "strategies/avx2/picture-avx2.h"
#include "strategies/avx2/quant-avx2.h"
#include "strategies/avx2/sao-avx2.h"
#include "strategies/sse2/picture-sse2.h"
#include "strategies/sse41/picture-sse41.h"

int kvz_strategy_register_picture_altivec(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_encode_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_dct_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_intra_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_quant_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_picture_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_ipol_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_sao_avx2(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_picture_sse2(void* opaque, unsigned char bitdepth) { (void)opaque; (void)bitdepth; return 1; }
int kvz_strategy_register_picture_sse41(void* opaque, uint8_t bitdepth) { (void)opaque; (void)bitdepth; return 1; }
