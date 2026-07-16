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

#ifndef _MINIOS_ARCH_X86_64_TSS_H_
#define _MINIOS_ARCH_X86_64_TSS_H_

#include <miniOS/types.h>

struct tss_struct {
    uint32_t reserved1;
    uint64_t privileged_stack_table[3];
    uint64_t reserved2;
    uint64_t interrupt_stack_table[7];
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t io_map_base_addr;
} __attribute__((__packed__));

/* Update TSS RSP0 (kernel stack pointer for ring-3->ring-0 transitions). */
void tss_set_rsp0(uint64_t rsp0);

#endif
