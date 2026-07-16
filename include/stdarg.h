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

/* Partial-include protocol used by Newlib and glibc headers.
 * When __need___va_list is defined before including <stdarg.h>, only
 * __gnuc_va_list is requested (not the full va_list/va_start/va_arg set).
 * This must be handled before the include guard so it works on re-include. */
#ifdef __need___va_list
  typedef __builtin_va_list __gnuc_va_list;
# undef __need___va_list
#endif

#ifndef _STDARG_H_
#define _STDARG_H_

typedef __builtin_va_list va_list;
#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)

#if 0
typedef char* va_list;

#define __va_size(type) \
  (((sizeof(type) + sizeof(long) - 1) / sizeof(long)) * sizeof(long))

#define va_start(ap, last) \
  ((ap) = (va_list) &(last) + __va_size(last))

#define va_arg(ap, type) \
  (*(type *)((ap) += __va_size(type), (ap) - __va_size(type)))
#endif

#endif
