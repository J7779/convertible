/******************************************************************************
*
* Copyright (C) 2012 Ittiam Systems Pvt Ltd, Bangalore
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
******************************************************************************/
/*
 * Portable (plain C) platform macros. Derived from
 * common/riscv64/ihevc_platform_macros.h of libhevc; the compiler builtins are
 * replaced by portable C so that the generic C paths build with every
 * compiler (GCC, Clang, MSVC, Emscripten) on 32- and 64-bit targets.
 * Behaviour is identical to the upstream definitions, including CLZ(0) == 31.
 */
#ifndef _IHEVC_PLATFORM_MACROS_H_
#define _IHEVC_PLATFORM_MACROS_H_

#include <string.h>

#define CLIP_U8(x) CLIP3((x), 0,     255)
#define CLIP_S8(x) CLIP3((x), -128,  127)

#define CLIP_U10(x) CLIP3((x), 0,     1023)
#define CLIP_S10(x) CLIP3((x), -512,  511)

#define CLIP_U12(x) CLIP3((x), 0,     4095)
#define CLIP_S12(x) CLIP3((x), -2048,  2047)

#define CLIP_U14(x) CLIP3((x), 0,     16383)
#define CLIP_S14(x) CLIP3((x), -8192,  8191)

#define CLIP_U16(x) CLIP3((x), 0,        65535)
#define CLIP_S16(x) CLIP3((x), -32768,   32767)

#define ITT_BIG_ENDIAN(x)   ((((UWORD32)(x) & 0x000000ffu) << 24) | \
                             (((UWORD32)(x) & 0x0000ff00u) << 8)  | \
                             (((UWORD32)(x) & 0x00ff0000u) >> 8)  | \
                             ((UWORD32)(x) >> 24))

#define SHL(x,y) (((y) < 32) ? ((x) << (y)) : 0)
#define SHR(x,y) (((y) < 32) ? ((x) >> (y)) : 0)

#define SHR_NEG(val,shift)  ((shift>0)?(val>>shift):(val<<(-shift)))
#define SHL_NEG(val,shift)  ((shift<0)?(val>>(-shift)):(val<<shift))

#define INLINE __inline

/* Number of leading zero bits of a non-zero 32-bit word. */
static __inline UWORD32 ihevc_portable_clz32(UWORD32 x)
{
    UWORD32 n = 0;
    if(!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if(!(x & 0xFF000000u)) { n += 8;  x <<= 8;  }
    if(!(x & 0xF0000000u)) { n += 4;  x <<= 4;  }
    if(!(x & 0xC0000000u)) { n += 2;  x <<= 2;  }
    if(!(x & 0x80000000u)) { n += 1; }
    return n;
}

static __inline UWORD32 ihevc_portable_clz64(ULWORD64 x)
{
    UWORD32 hi = (UWORD32)(x >> 32);
    if(hi)
        return ihevc_portable_clz32(hi);
    return 32 + ihevc_portable_clz32((UWORD32)x);
}

static __inline UWORD32 ihevc_portable_ctz32(UWORD32 x)
{
    UWORD32 n = 0;
    if(!(x & 0x0000FFFFu)) { n += 16; x >>= 16; }
    if(!(x & 0x000000FFu)) { n += 8;  x >>= 8;  }
    if(!(x & 0x0000000Fu)) { n += 4;  x >>= 4;  }
    if(!(x & 0x00000003u)) { n += 2;  x >>= 2;  }
    if(!(x & 0x00000001u)) { n += 1; }
    return n;
}

static __inline UWORD32 ihevc_portable_popcount32(UWORD32 x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
}

#define POPCNT_U32(x)       ihevc_portable_popcount32((UWORD32)(x))

static __inline UWORD32 CLZ(UWORD32 u4_word)
{
    if(u4_word)
        return ihevc_portable_clz32(u4_word);
    else
        return 31;
}

static __inline UWORD32 CLZNZ(UWORD32 u4_word)
{
    return ihevc_portable_clz32(u4_word);
}

static __inline UWORD32 CTZ(UWORD32 u4_word)
{
    if(0 == u4_word)
        return 31;
    else
        return ihevc_portable_ctz32(u4_word);
}

/* Single-threaded build: no memory barrier is needed. */
#define DATA_SYNC()

#define GET_POS_MSB_32(r,word)                         \
{                                                      \
    if(word)                                           \
    {                                                  \
        r = 31 - (WORD32)ihevc_portable_clz32(word);   \
    }                                                  \
    else                                               \
    {                                                  \
        r = -1;                                        \
    }                                                  \
}

#define GET_POS_MSB_64(r,word)                         \
{                                                      \
    if(word)                                           \
    {                                                  \
        r = 63 - (WORD32)ihevc_portable_clz64(word);   \
    }                                                  \
    else                                               \
    {                                                  \
        r = -1;                                        \
    }                                                  \
}

#define GETRANGE(r,word)                               \
{                                                      \
    if(word)                                           \
    {                                                  \
        r = 32 - (WORD32)ihevc_portable_clz32(word);   \
    }                                                  \
    else                                               \
    {                                                  \
        r = 1;                                         \
    }                                                  \
}

#define GETRANGE64(r,llword)                             \
{                                                        \
    if(llword)                                           \
    {                                                    \
        r = 64 - (WORD32)ihevc_portable_clz64(llword);   \
    }                                                    \
    else                                                 \
    {                                                    \
        r = 1;                                           \
    }                                                    \
}

/* Unaligned-safe loads and stores (replacing casts such as *(UWORD32 *)pu1,
 * which fault on strict-alignment targets and are undefined behaviour). */
static __inline UWORD32 ihevc_load_u32(const void *p)
{
    UWORD32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static __inline UWORD16 ihevc_load_u16(const void *p)
{
    UWORD16 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static __inline void ihevc_store_u32(void *p, UWORD32 v)
{
    memcpy(p, &v, sizeof(v));
}

static __inline void ihevc_store_u16(void *p, UWORD16 v)
{
    memcpy(p, &v, sizeof(v));
}

#define NOP(nop_cnt)
#define PLD(a)

/* Alignment only matters for the SIMD paths, which are not built. */
#define MEM_ALIGN8
#define MEM_ALIGN16
#define MEM_ALIGN32

#endif /* _IHEVC_PLATFORM_MACROS_H_ */
