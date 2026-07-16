// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

/**
 * @file cc.h
 * @brief lwIP compiler abstraction for miniOS x86_64-elf target.
 *
 * Provides type definitions, byte order, struct packing macros, and
 * diagnostic hooks required by lwIP's internal headers. Designed for
 * NO_SYS=1 (poll-driven) mode — no locking primitives needed.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef __ssize_t_defined
typedef long ssize_t;
#define __ssize_t_defined
#endif

/* Tell lwIP arch.h not to re-typedef ssize_t or pull in unistd.h */
#ifndef SSIZE_MAX
#define SSIZE_MAX __LONG_MAX__
#endif

/* lwIP basic types — x86_64-elf is LP64, little-endian */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* Struct packing — GCC attribute */
#define PACK_STRUCT_FIELD(x)   x
#define PACK_STRUCT_STRUCT     __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* NO_SYS=1: single-threaded poll loop, no locking needed */
#define SYS_ARCH_DECL_PROTECT(lev)
#define SYS_ARCH_PROTECT(lev)
#define SYS_ARCH_UNPROTECT(lev)

/* Diagnostics — route through kernel printk */
#ifndef _MINIOS_PRINTK_H_
extern int printk(const char *fmt, ...);
#endif
#define LWIP_PLATFORM_DIAG(x)   do { printk x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { printk("lwIP ASSERT: %s\n", x); for(;;); } while(0)

#endif /* LWIP_ARCH_CC_H */
