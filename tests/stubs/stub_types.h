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

/* stub_types.h — host-compatible replacement for miniOS/types.h
 * This file is force-included before any kernel headers so that the
 * real types.h guard fires and skips the kernel version.
 *
 * Include stdarg.h first so __gnuc_va_list is defined before stdio.h
 * uses it (needed because stub_printk.h includes stdio.h). */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#ifndef _MINIOS_TYPES_H_
#define _MINIOS_TYPES_H_

/* Re-define size_t as uint64_t to match the kernel's definition.
 * stddef.h may have defined size_t differently on 64-bit hosts
 * (unsigned long vs unsigned long long) — undefine and redefine. */
#undef size_t
typedef uint64_t size_t;

/* Other kernel types */
typedef uint64_t addr_t;
typedef uint64_t loffset_t;
typedef uint32_t offset_t;
typedef uint64_t time_t;
typedef int32_t  pid_t;
typedef uint16_t wint_t;

/* Satisfy any code that includes miniOS/types.h for these typedefs */
typedef void (*irq_handler_t)(uint64_t rsp);
typedef void (*exception_handler_t)(void *context);
typedef void (*task_entry_t)(void);

/* Do NOT redefine uintptr_t: the system stdint.h already defines it
 * as unsigned long (correct on 64-bit hosts). The kernel types.h
 * defines it as "unsigned int*" which is non-standard; we skip that. */

#ifndef NULL
#define NULL ((void *)0)
#endif

#define INT_MAX  32767
#define INT_MIN  -32767

typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;
typedef int64_t  ptrdiff_t;

#endif /* _MINIOS_TYPES_H_ */
