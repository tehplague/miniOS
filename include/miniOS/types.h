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

#ifndef _MINIOS_TYPES_H_
#define _MINIOS_TYPES_H_

// Define basic types of well-known length
typedef signed char int8_t;
typedef unsigned char uint8_t;

typedef signed short int16_t;
typedef unsigned short uint16_t;

typedef signed int int32_t;
typedef unsigned int uint32_t;

typedef signed long int int64_t;
typedef unsigned long int uint64_t;

typedef uint64_t addr_t;
typedef uint64_t size_t;
typedef uint32_t offset_t;
typedef uint64_t loffset_t;
typedef int32_t pid_t;

// IRQ handler type
typedef void (*irq_handler_t)(uint64_t rsp);
// Exception handler type
typedef void (*exception_handler_t)(void *context);
// Kernel thread entry point
typedef void (*task_entry_t)(void);

#define NULL    ((void *)0)

#define INT_MAX     32767
#define INT_MIN     -32767

typedef int64_t intmax_t;
typedef uint64_t uintmax_t;
typedef unsigned long uintptr_t;
typedef signed long ptrdiff_t;

#endif
