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

/* stub_elf.c — fake ELF loader for host-native unit tests.
 * Used by test_syscall.c which includes syscall.c (SYS_execve calls elf_load).
 * NOT used by test_elf.c (which includes elf.c directly with inline VFS stubs). */

#include <miniOS/fs/elf.h>

int elf_load(int fd, uint64_t *entry_out, uint64_t *image_end_out,
             uint64_t *phdr_va_out, uint16_t *phnum_out) {
    (void)fd;
    if (entry_out)    *entry_out    = 0x400000;
    if (image_end_out)*image_end_out= 0x402000;
    if (phdr_va_out)  *phdr_va_out  = 0;
    if (phnum_out)    *phnum_out    = 0;
    return 0;
}
