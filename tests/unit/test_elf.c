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

/* test_elf.c — ELF header validation unit tests.
 * Includes elf.c directly. Provides inline stubs for:
 *   - vfs_read/vfs_open/vfs_close/vfs_mount/vfs_readdir (sequential buffer)
 *   - pmm_alloc_frame / vmm_map_page / vmm_unmap_page
 *   - memset (from host string.h)
 *
 * Linked with: unity.c, stub_sched.c, stub_keyboard.c
 * NOT linked with: stub_vfs.c, stub_elf.c, stub_pmm.c, stub_vmm.c
 * (all these are provided inline below) */

#include "unity.h"
#include <string.h>
#include <stdint.h>

/* Pull in VFS types (vfs_ops_t, vfs_dirent_cb_t) before defining stubs.
 * These are needed for the vfs_readdir and vfs_mount stub signatures. */
#include <miniOS/fs/vfs.h>

#define TEST_ELF_FD VFS_FIRST_OPEN_FD

/* -----------------------------------------------------------------------
 * Sequential VFS stub: backed by a static buffer.
 * elf.c calls vfs_read sequentially from offset 0.
 * --------------------------------------------------------------------- */
static const uint8_t *elf_vfs_buf = NULL;
static uint32_t elf_vfs_buf_size  = 0;
static uint32_t elf_vfs_read_pos  = 0;

/* Set the buffer to use for the next elf_load call */
static void elf_set_buf(const uint8_t *buf, uint32_t size) {
    elf_vfs_buf      = buf;
    elf_vfs_buf_size = size;
    elf_vfs_read_pos = 0;
}

/* vfs_read: copies sequentially from the buffer */
int vfs_read(int fd, void *out, uint32_t len) {
    (void)fd;
    if (!elf_vfs_buf) return -1;
    uint32_t avail = elf_vfs_buf_size - elf_vfs_read_pos;
    uint32_t n = (len < avail) ? len : avail;
    if (n == 0) return 0;
    memcpy(out, elf_vfs_buf + elf_vfs_read_pos, n);
    elf_vfs_read_pos += n;
    return (int)n;
}

int vfs_open(const char *path, int flags, int mode)                { (void)path; (void)flags; (void)mode; return TEST_ELF_FD; }
int vfs_close(int fd)                                              { (void)fd;   return 0; }
int vfs_readdir(int fd, vfs_dirent_cb_t cb, void *ud)            { (void)fd; (void)cb; (void)ud; return 0; }
void vfs_mount(vfs_ops_t *ops)                                     { (void)ops; }

/* -----------------------------------------------------------------------
 * PMM / VMM stubs (elf.c calls pmm_alloc_frame + vmm_map_page + memset)
 * memset: use host <string.h> version already included above.
 * For vmm_map_page: the page_va argument is a user VA like 0x400000 which
 * IS accessible on the host (it's in userspace). We just allow the writes.
 * --------------------------------------------------------------------- */
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>

/* We need PAGE_PRESENT, PAGE_WRITE, PAGE_USER — from vmm.h above */

uint64_t pmm_alloc_frame(void) {
    /* Return a dummy non-zero physical address.
     * elf.c passes this to vmm_map_page which we ignore. */
    static uint64_t counter = 1;
    return (counter++) * 0x1000ULL;
}

void pmm_free_frame(uint64_t phys) { (void)phys; }
uint64_t pmm_free_count(void) { return 256; }

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)virt; (void)phys; (void)flags;
    return 0;   /* success — host already has the pages mapped */
}

void vmm_unmap_page(uint64_t virt) { (void)virt; }

uint64_t vmm_virt_to_phys(uint64_t virt) { (void)virt; return 0; /* unmapped */ }

/* -----------------------------------------------------------------------
 * Now pull in the real ELF implementation.
 * elf.c uses our vfs_read above (not stub_vfs.c) and our pmm/vmm stubs.
 * --------------------------------------------------------------------- */
#include "../../src/kernel/fs/elf.c"

/* -----------------------------------------------------------------------
 * Build a minimal valid ELF binary in a buffer.
 * We set e_phnum=1 and the single PT_LOAD has p_filesz=0 p_memsz=0x1000
 * so elf.c maps pages but reads no segment data.
 * We also place p_vaddr at a user address (0x400000) which the host can
 * write to after vmm_map_page (which is a no-op here).
 * NOTE: elf.c will still call memset((void *)p_vaddr, 0, 4096).
 * To avoid a segfault at 0x400000, we set p_vaddr to point to our own
 * stack buffer so the memset hits real memory.
 * --------------------------------------------------------------------- */
static uint8_t segment_page[4096] __attribute__((aligned(4096)));

typedef struct {
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;
} __attribute__((packed)) MinimalElf;

static MinimalElf build_valid_elf(void) {
    MinimalElf e;
    memset(&e, 0, sizeof(e));

    Elf64_Ehdr *h = &e.ehdr;
    h->e_ident[0] = 0x7f;
    h->e_ident[1] = 'E';
    h->e_ident[2] = 'L';
    h->e_ident[3] = 'F';
    h->e_type      = ET_EXEC;
    h->e_machine   = EM_X86_64;
    h->e_version   = 1;
    h->e_entry     = 0x401000;
    h->e_phoff     = sizeof(Elf64_Ehdr);
    h->e_ehsize    = sizeof(Elf64_Ehdr);
    h->e_phentsize = sizeof(Elf64_Phdr);
    h->e_phnum     = 1;

    Elf64_Phdr *p = &e.phdr;
    p->p_type   = PT_LOAD;
    p->p_flags  = PF_R | PF_X;
    p->p_offset = sizeof(MinimalElf);   /* segment data at end of buffer */
    p->p_vaddr  = (uint64_t)(uintptr_t)segment_page;  /* host address — safe */
    p->p_paddr  = (uint64_t)(uintptr_t)segment_page;
    p->p_filesz = 0;       /* no file data to read */
    p->p_memsz  = 4096;    /* one page */
    p->p_align  = 4096;

    return e;
}

/* -----------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------- */
void setUp(void) {
    elf_vfs_buf      = NULL;
    elf_vfs_buf_size = 0;
    elf_vfs_read_pos = 0;
    memset(segment_page, 0, sizeof(segment_page));
}

void tearDown(void) { /* nothing */ }

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/* valid ELF header (magic+ET_EXEC+EM_X86_64) + 1 PT_LOAD — elf_load returns 0 */
void test_valid_elf_returns_zero(void) {
    MinimalElf e = build_valid_elf();
    elf_set_buf((const uint8_t *)&e, sizeof(e));
    uint64_t entry = 0;
    int r = elf_load(TEST_ELF_FD, &entry, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT64(0x401000, entry);
}

/* bad magic bytes — elf_load returns -1 */
void test_bad_magic_returns_minus1(void) {
    MinimalElf e = build_valid_elf();
    e.ehdr.e_ident[0] = 'X';   /* corrupt magic */
    elf_set_buf((const uint8_t *)&e, sizeof(e));
    uint64_t entry = 0;
    int r = elf_load(TEST_ELF_FD, &entry, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* wrong e_type (not ET_EXEC) — returns -1 */
void test_wrong_type_returns_minus1(void) {
    MinimalElf e = build_valid_elf();
    e.ehdr.e_type = 4;  /* ET_CORE — invalid for loading */
    elf_set_buf((const uint8_t *)&e, sizeof(e));
    uint64_t entry = 0;
    int r = elf_load(TEST_ELF_FD, &entry, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* wrong e_machine (not EM_X86_64) — returns -1 */
void test_wrong_machine_returns_minus1(void) {
    MinimalElf e = build_valid_elf();
    e.ehdr.e_machine = 3;   /* EM_386 instead of EM_X86_64 */
    elf_set_buf((const uint8_t *)&e, sizeof(e));
    uint64_t entry = 0;
    int r = elf_load(TEST_ELF_FD, &entry, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* e_phnum=0 — returns -1 */
void test_zero_phdrs_returns_minus1(void) {
    MinimalElf e = build_valid_elf();
    e.ehdr.e_phnum = 0;
    elf_set_buf((const uint8_t *)&e, sizeof(e));
    uint64_t entry = 0;
    int r = elf_load(TEST_ELF_FD, &entry, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* wrong e_phentsize — returns -1 */
void test_wrong_phentsize_returns_minus1(void) {
    MinimalElf e = build_valid_elf();
    e.ehdr.e_phentsize = 32;  /* wrong size */
    elf_set_buf((const uint8_t *)&e, sizeof(e));
    uint64_t entry = 0;
    int r = elf_load(TEST_ELF_FD, &entry, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_elf_returns_zero);
    RUN_TEST(test_bad_magic_returns_minus1);
    RUN_TEST(test_wrong_type_returns_minus1);
    RUN_TEST(test_wrong_machine_returns_minus1);
    RUN_TEST(test_zero_phdrs_returns_minus1);
    RUN_TEST(test_wrong_phentsize_returns_minus1);
    return UNITY_END();
}
