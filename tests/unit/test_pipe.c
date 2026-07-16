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

/* test_pipe.c — unit tests for pipe ring-buffer implementation.
 * Includes pipe.c directly to test pipe_create/read/write/free.
 *
 * Host-test note: pipe_read/pipe_write take (uint32_t ino) where ino is a
 * pipe_t * truncated to 32 bits. This works on miniOS (128MB RAM, all heap
 * addresses < 4GB) but not on the host where malloc returns high addresses.
 * We use mmap(MAP_FIXED) at a low address so the pointer fits in 32 bits. */

#include "unity.h"
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

/* vfs.h must be included before pipe.c to provide the vfs_ops_t typedef
 * (pipe.c defines vfs_ops_t pipe_ops = {...}). The stub build system provides
 * all necessary types via stub_types.h and stub_printk.h (force-included). */
#include <miniOS/fs/vfs.h>

/* Low-address mmap region for pipe_t objects.
 * We carve a 64KB region starting at 0x100000 and use it as a simple bump
 * allocator so that all pipe_t pointers fit in uint32_t. */
#define PIPE_TEST_BASE   0x100000UL   /* 1 MB — below any normal heap */
#define PIPE_TEST_SIZE   0x10000UL    /* 64 KB — room for ~16 pipe_t objects */
static uint8_t *g_pipe_arena    = NULL;
static size_t   g_pipe_arena_off = 0;

static void pipe_arena_init(void) {
    if (g_pipe_arena) return;
    g_pipe_arena = mmap((void *)PIPE_TEST_BASE, PIPE_TEST_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
    if (g_pipe_arena == MAP_FAILED) {
        /* Fallback: try any low-address mmap if MAP_FIXED_NOREPLACE fails */
        g_pipe_arena = mmap(NULL, PIPE_TEST_SIZE,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1, 0);
    }
}

/* Provide kmalloc/kfree for host test — allocate from low-address arena */
static int stub_kmalloc_fail = 0;

void *kmalloc(size_t size) {
    if (stub_kmalloc_fail) return NULL;
    pipe_arena_init();
    if (!g_pipe_arena || g_pipe_arena == MAP_FAILED) return NULL;
    /* Align to 8 bytes */
    size_t aligned_off = (g_pipe_arena_off + 7) & ~7UL;
    if (aligned_off + size > PIPE_TEST_SIZE) return NULL;
    void *ptr = g_pipe_arena + aligned_off;
    g_pipe_arena_off = aligned_off + size;
    return ptr;
}

void kfree(void *ptr) {
    (void)ptr;
    /* No-op: arena allocator; memory is reused across tests via tearDown reset */
}

/* Include the real pipe implementation */
#include "../../src/kernel/fs/pipe.c"

/* -----------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------- */
void setUp(void) {
    stub_kmalloc_fail = 0;
    /* Reset arena bump pointer — reuse low-address region per test */
    g_pipe_arena_off = 0;
    /* Clear arena content to avoid stale state bleed between tests */
    if (g_pipe_arena && g_pipe_arena != MAP_FAILED)
        memset(g_pipe_arena, 0, PIPE_TEST_SIZE);
}

void tearDown(void) {
    /* nothing */
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/* pipe_create() returns initialized pipe_t */
void test_pipe_create_returns_initialized_pipe(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(0, p->ref_count);
    TEST_ASSERT_EQUAL_UINT32(0, p->write_pos);
    TEST_ASSERT_EQUAL_UINT32(0, p->read_pos);
    TEST_ASSERT_EQUAL_UINT8(0, p->write_closed);
    kfree(p);
}

/* pipe_create() returns NULL when kmalloc fails */
void test_pipe_create_returns_null_on_oom(void) {
    stub_kmalloc_fail = 1;
    pipe_t *p = pipe_create();
    TEST_ASSERT_NULL(p);
}

/* pipe_write: write 5 bytes at start, advances write_pos */
void test_pipe_write_basic(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    const char msg[] = "hello";
    int n = pipe_write((uint32_t)(uintptr_t)p, 0, msg, 5);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_UINT32(5, p->write_pos);
    TEST_ASSERT_EQUAL_UINT32(0, p->read_pos);
    kfree(p);
}

/* pipe_read: read 5 bytes that were written, advances read_pos */
void test_pipe_read_basic(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    const char msg[] = "hello";
    pipe_write((uint32_t)(uintptr_t)p, 0, msg, 5);
    char buf[8] = {0};
    int n = pipe_read((uint32_t)(uintptr_t)p, 0, buf, 8);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_MEMORY(msg, buf, 5);
    TEST_ASSERT_EQUAL_UINT32(5, p->read_pos);
    kfree(p);
}

/* pipe_read: read from empty pipe with write_closed=0 returns 0 */
void test_pipe_read_empty_no_eof_returns_zero(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    char buf[8] = {0};
    int n = pipe_read((uint32_t)(uintptr_t)p, 0, buf, 8);
    TEST_ASSERT_EQUAL_INT(0, n);
    kfree(p);
}

/* pipe_read: read from empty pipe with write_closed=1 returns 0 (EOF) */
void test_pipe_read_empty_write_closed_returns_eof(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    p->write_closed = 1;
    char buf[8] = {0};
    int n = pipe_read((uint32_t)(uintptr_t)p, 0, buf, 8);
    TEST_ASSERT_EQUAL_INT(0, n);
    kfree(p);
}

/* pipe_write: full buffer returns 0 (partial write count of 0) */
void test_pipe_write_full_buffer_returns_zero(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    /* Fill the buffer completely: write_pos - read_pos == PIPE_SIZE */
    p->write_pos = PIPE_SIZE;
    p->read_pos  = 0;
    const char msg[] = "x";
    int n = pipe_write((uint32_t)(uintptr_t)p, 0, msg, 1);
    TEST_ASSERT_EQUAL_INT(0, n);
    kfree(p);
}

/* pipe_free: ref_count=1 -> decrements to 0, frees (no crash) */
void test_pipe_free_last_ref_frees(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    p->ref_count = 1;
    pipe_free(p);  /* should call kfree(p) — no crash means pass */
}

/* pipe_free: ref_count=2 -> decrements to 1, does NOT free */
void test_pipe_free_non_last_ref_keeps_pipe(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);
    p->ref_count = 2;
    pipe_free(p);
    TEST_ASSERT_EQUAL_UINT32(1, p->ref_count);
    /* Still alive — free it now */
    p->ref_count = 1;
    pipe_free(p);
}

/* pipe_free(NULL): safe no-op */
void test_pipe_free_null_is_safe(void) {
    pipe_free(NULL);  /* must not crash */
}

/* Wraparound: write 4090 bytes, read 4090, write 10 more — wraps correctly */
void test_pipe_wraparound(void) {
    pipe_t *p = pipe_create();
    TEST_ASSERT_NOT_NULL(p);

    /* Use heap buffers to avoid large stack allocations */
    uint8_t *src = malloc(4090);
    uint8_t *dst = malloc(4090);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    /* Phase 1: write 4090 bytes with value 0xAA */
    memset(src, 0xAA, 4090);
    int n = pipe_write((uint32_t)(uintptr_t)p, 0, src, 4090);
    TEST_ASSERT_EQUAL_INT(4090, n);
    TEST_ASSERT_EQUAL_UINT32(4090, p->write_pos);

    /* Phase 2: read 4090 bytes, verify content */
    memset(dst, 0, 4090);
    int r = pipe_read((uint32_t)(uintptr_t)p, 0, dst, 4090);
    TEST_ASSERT_EQUAL_INT(4090, r);
    TEST_ASSERT_EQUAL_UINT32(4090, p->read_pos);
    for (int i = 0; i < 4090; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, dst[i]);
    }

    free(src);
    free(dst);

    /* Phase 3: write 10 bytes with value 0xBB — write_pos wraps in buf[] indexing */
    uint8_t src2[10];
    memset(src2, 0xBB, sizeof(src2));
    int n2 = pipe_write((uint32_t)(uintptr_t)p, 0, src2, 10);
    TEST_ASSERT_EQUAL_INT(10, n2);
    /* write_pos is monotonically increasing: 4090 + 10 = 4100 */
    TEST_ASSERT_EQUAL_UINT32(4100, p->write_pos);

    /* Phase 4: read those 10 bytes back */
    uint8_t dst2[10] = {0};
    int r2 = pipe_read((uint32_t)(uintptr_t)p, 0, dst2, 10);
    TEST_ASSERT_EQUAL_INT(10, r2);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xBB, dst2[i]);
    }

    kfree(p);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pipe_create_returns_initialized_pipe);
    RUN_TEST(test_pipe_create_returns_null_on_oom);
    RUN_TEST(test_pipe_write_basic);
    RUN_TEST(test_pipe_read_basic);
    RUN_TEST(test_pipe_read_empty_no_eof_returns_zero);
    RUN_TEST(test_pipe_read_empty_write_closed_returns_eof);
    RUN_TEST(test_pipe_write_full_buffer_returns_zero);
    RUN_TEST(test_pipe_free_last_ref_frees);
    RUN_TEST(test_pipe_free_non_last_ref_keeps_pipe);
    RUN_TEST(test_pipe_free_null_is_safe);
    RUN_TEST(test_pipe_wraparound);
    return UNITY_END();
}
