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

/* tests/stubs/stdarg.h — override the kernel's include/stdarg.h for host builds.
 * The kernel's include/stdarg.h lacks __gnuc_va_list which glibc's stdio.h needs.
 * We directly typedef __gnuc_va_list from the GCC built-in here, then define
 * the standard va_* macros consistently. */
#ifndef _STUB_STDARG_H_
#define _STUB_STDARG_H_

/* Also satisfy the kernel's own guard so include/stdarg.h is skipped */
#ifndef _STDARG_H_
#define _STDARG_H_
#endif

/* Define __gnuc_va_list as required by glibc's stdio.h */
typedef __builtin_va_list __gnuc_va_list;

/* Define va_list as the standard alias */
typedef __gnuc_va_list va_list;

/* Standard va_* macros */
#define va_start(v, l)  __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v, l)    __builtin_va_arg(v, l)
#define va_copy(d, s)   __builtin_va_copy(d, s)

#endif /* _STUB_STDARG_H_ */
