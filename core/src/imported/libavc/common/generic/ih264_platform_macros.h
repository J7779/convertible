/******************************************************************************
 *
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *****************************************************************************
 * Originally developed and contributed by Ittiam Systems Pvt. Ltd, Bangalore
*/
/**
*******************************************************************************
* @file
*  ih264_platform_macros.h
*
* @brief
*  Platform specific Macro definitions used in the codec
*
* @author
*  Ittiam
*
* @remarks
*  None
*
*******************************************************************************
*/


#ifndef _IH264_PLATFORM_MACROS_H_
#define _IH264_PLATFORM_MACROS_H_

#include <stdint.h>

#define CLIP_U8(x) CLIP3(0, UINT8_MAX, (x))
#define CLIP_S8(x) CLIP3(INT8_MIN, INT8_MAX, (x))

#define CLIP_U10(x) CLIP3(0, 1023, (x))
#define CLIP_S10(x) CLIP3(-512, 511, (x))

#define CLIP_U11(x) CLIP3(0, 2047, (x))
#define CLIP_S11(x) CLIP3(-1024, 1023, (x))

#define CLIP_U12(x) CLIP3(0, 4095, (x))
#define CLIP_S12(x) CLIP3(-2048, 2047, (x))

#define CLIP_U16(x) CLIP3(0, UINT16_MAX, (x))
#define CLIP_S16(x) CLIP3(INT16_MIN, INT16_MAX, (x))

#define CLIP_U32(x) CLIP3(0, UINT32_MAX, (x))
#define CLIP_S32(x) CLIP3(INT32_MIN, INT32_MAX, (x))

#define SHL(x,y) (((y) < 32) ? ((x) << (y)) : 0)
#define SHR(x,y) (((y) < 32) ? ((x) >> (y)) : 0)

#define SHR_NEG(val,shift)  ((shift>0)?(val>>shift):(val<<(-shift)))
#define SHL_NEG(val,shift)  ((shift<0)?(val>>(-shift)):(val<<shift))


/* Portable C version (no compiler builtins, no inline assembly) used by
 * Convertible for every target: derived from the riscv/ variant, which is
 * already free of SIMD. The decoder and encoder run single-threaded, so the
 * memory barrier and the spin-wait NOP are no-ops. */

#define ITT_BIG_ENDIAN(x) \
    ((((UWORD32)(x) & 0x000000FFu) << 24) | (((UWORD32)(x) & 0x0000FF00u) << 8) | \
     (((UWORD32)(x) & 0x00FF0000u) >> 8) | (((UWORD32)(x) & 0xFF000000u) >> 24))

#define NOP(nop_cnt)

#define PLD(a)

/* In normal cases, 0 will not be passed as an argument to CLZ and CTZ.
As CLZ and CTZ outputs are used as a shift value in few places, these return
31 for u4_word == 0 case, just to handle error cases gracefully without any
undefined behaviour */

static __inline UWORD32 CLZ(UWORD32 u4_word)
{
    UWORD32 n = 0;
    if(0 == u4_word)
        return 31;
    if(0 == (u4_word & 0xFFFF0000u)) { n += 16; u4_word <<= 16; }
    if(0 == (u4_word & 0xFF000000u)) { n += 8; u4_word <<= 8; }
    if(0 == (u4_word & 0xF0000000u)) { n += 4; u4_word <<= 4; }
    if(0 == (u4_word & 0xC0000000u)) { n += 2; u4_word <<= 2; }
    if(0 == (u4_word & 0x80000000u)) { n += 1; }
    return n;
}

static __inline UWORD32 CTZ(UWORD32 u4_word)
{
    UWORD32 n = 0;
    if(0 == u4_word)
        return 31;
    if(0 == (u4_word & 0x0000FFFFu)) { n += 16; u4_word >>= 16; }
    if(0 == (u4_word & 0x000000FFu)) { n += 8; u4_word >>= 8; }
    if(0 == (u4_word & 0x0000000Fu)) { n += 4; u4_word >>= 4; }
    if(0 == (u4_word & 0x00000003u)) { n += 2; u4_word >>= 2; }
    if(0 == (u4_word & 0x00000001u)) { n += 1; }
    return n;
}

#define DATA_SYNC()

#define INLINE __inline

#if defined(_MSC_VER)
#define MEM_ALIGN8 __declspec(align(8))
#define MEM_ALIGN16 __declspec(align(16))
#define MEM_ALIGN32 __declspec(align(32))
#else
#define MEM_ALIGN8 __attribute__ ((aligned (8)))
#define MEM_ALIGN16 __attribute__ ((aligned (16)))
#define MEM_ALIGN32 __attribute__ ((aligned (32)))
#endif

#endif /* _IH264_PLATFORM_MACROS_H_ */
