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

/* stub_vmm.c — fake VMM for host-native unit tests.
 * vmm_map_page is a no-op because the heap backing buffer is already
 * mapped by the host OS. Returns 0 (success) unconditionally. */

#include <miniOS/mm/vmm.h>

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt;
    (void)phys;
    (void)flags;
    return 0;
}

void vmm_unmap_page(uint64_t virt) {
    (void)virt;
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    (void)virt;
    return 0;
}
