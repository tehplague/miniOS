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

/* test_tmpfs.c — unit tests for the tmpfs in-memory filesystem.
 * Tests TMPFS-01 through TMPFS-04 (create, read/write, unlink, mkdir/rmdir).
 * Directly #includes tmpfs.c; provides kmalloc/kfree stubs via malloc/free.
 * TMPFS-05 (data lost on reboot) is verified by the absence of any disk I/O
 * in the implementation — confirmed by code inspection, not a runtime test.
 *
 * stub_types.h and stub_printk.h are force-included by TEST_CFLAGS, providing
 * integer types and #define printk printf. */

#include "unity.h"
#include <string.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * kmalloc/kfree stubs: delegate to host malloc/free.
 * Must be defined before #include of tmpfs.c so the translation unit
 * sees these definitions instead of the kernel heap functions.
 * Declared as static to avoid collisions when linking with other TUs.
 * --------------------------------------------------------------------- */
static inline void *kmalloc(size_t size) { return malloc(size); }
static inline void  kfree(void *ptr)     { free(ptr); }

/* Satisfy the heap.h include guard so the real heap.h is not pulled in
 * and does not redeclare kmalloc/kfree with external linkage. */
#define _MINIOS_MM_HEAP_H_

/* Include tmpfs implementation directly (pulls in tmpfs.h -> vfs.h -> ext2.h) */
#include "../../src/kernel/fs/tmpfs.c"

/* Stubs for vfs.c functions called by tmpfs_mount() / tmpfs_init().
 * Defined after the include so that types from vfs.h are available. */
int vfs_register_mount(const char *point, vfs_ops_t *ops, uint32_t root_ino) {
    (void)point; (void)ops; (void)root_ino; return 0;
}
int register_filesystem(const char *name,
                        int (*mount)(const char *, const char *, const void *)) {
    (void)name; (void)mount; return 0;
}

/* -----------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------- */
static int g_readdir_count;
static char g_readdir_names[16][256];

static int count_cb(const char *name, uint8_t name_len, uint32_t ino,
                    uint8_t ftype, void *ud) {
    (void)ino; (void)ftype; (void)ud;
    if (g_readdir_count < 16) {
        memcpy(g_readdir_names[g_readdir_count], name, name_len);
        g_readdir_names[g_readdir_count][name_len] = '\0';
    }
    g_readdir_count++;
    return 0;
}

void setUp(void) {
    /* Reset tmpfs state completely before each test */
    memset(g_tmpfs_inodes, 0, sizeof(g_tmpfs_inodes));
    g_tmpfs_next_ino = 1;
    g_tmpfs_initialized = 0;
    tmpfs_init();
    g_readdir_count = 0;
    memset(g_readdir_names, 0, sizeof(g_readdir_names));
}
void tearDown(void) {}

/* -----------------------------------------------------------------------
 * Tests: TMPFS-01 — create file
 * --------------------------------------------------------------------- */
void test_create_file_in_root(void) {
    uint32_t new_ino = 0;
    int rc = tmpfs_create(TMPFS_ROOT_INO, "hello.txt", 0, &new_ino);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_EQUAL(0, new_ino);
}

void test_create_file_lookup_succeeds(void) {
    uint32_t new_ino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "hello.txt", 0, &new_ino);
    vfs_inode_info_t info;
    int rc = tmpfs_lookup("hello.txt", &info);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(new_ino, info.inode);
    TEST_ASSERT_EQUAL_INT(VFS_FILE_TYPE_REG, info.file_type);
    TEST_ASSERT_EQUAL_UINT64(0, info.size);
}

void test_lookup_nonexistent_fails(void) {
    vfs_inode_info_t info;
    int rc = tmpfs_lookup("nosuchfile", &info);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_create_duplicate_fails(void) {
    uint32_t ino;
    tmpfs_create(TMPFS_ROOT_INO, "dup.txt", 0, &ino);
    uint32_t ino2;
    int rc = tmpfs_create(TMPFS_ROOT_INO, "dup.txt", 0, &ino2);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* -----------------------------------------------------------------------
 * Tests: TMPFS-02 — read/write
 * --------------------------------------------------------------------- */
void test_write_then_read(void) {
    uint32_t ino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "data.txt", 0, &ino);
    const char *msg = "hello tmpfs";
    int wn = tmpfs_write(ino, 0, msg, 11);
    TEST_ASSERT_EQUAL_INT(11, wn);
    char buf[32] = {0};
    int rn = tmpfs_read(ino, 0, buf, 11);
    TEST_ASSERT_EQUAL_INT(11, rn);
    TEST_ASSERT_EQUAL_STRING("hello tmpfs", buf);
}

void test_write_updates_size(void) {
    uint32_t ino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "sz.txt", 0, &ino);
    tmpfs_write(ino, 0, "abc", 3);
    vfs_inode_info_t info;
    tmpfs_lookup("sz.txt", &info);
    TEST_ASSERT_EQUAL_UINT64(3, info.size);
}

void test_read_past_eof_returns_zero(void) {
    uint32_t ino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "eof.txt", 0, &ino);
    tmpfs_write(ino, 0, "x", 1);
    char buf[8];
    int rn = tmpfs_read(ino, 100, buf, 8);
    TEST_ASSERT_EQUAL_INT(0, rn);
}

void test_write_across_block_boundary(void) {
    uint32_t ino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "big.txt", 0, &ino);
    /* Write 4097 bytes to cross the 4096-byte block boundary */
    char *data = malloc(4097);
    memset(data, 'A', 4097);
    int wn = tmpfs_write(ino, 0, data, 4097);
    TEST_ASSERT_EQUAL_INT(4097, wn);
    char *readbuf = malloc(4097);
    int rn = tmpfs_read(ino, 0, readbuf, 4097);
    TEST_ASSERT_EQUAL_INT(4097, rn);
    TEST_ASSERT_EQUAL_MEMORY(data, readbuf, 4097);
    free(data);
    free(readbuf);
}

/* -----------------------------------------------------------------------
 * Tests: TMPFS-03 — unlink
 * --------------------------------------------------------------------- */
void test_unlink_removes_file(void) {
    uint32_t ino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "rm.txt", 0, &ino);
    int rc = tmpfs_unlink(TMPFS_ROOT_INO, "rm.txt");
    TEST_ASSERT_EQUAL_INT(0, rc);
    vfs_inode_info_t info;
    int lr = tmpfs_lookup("rm.txt", &info);
    TEST_ASSERT_EQUAL_INT(-1, lr);
}

void test_unlink_nonexistent_fails(void) {
    int rc = tmpfs_unlink(TMPFS_ROOT_INO, "ghost.txt");
    TEST_ASSERT_EQUAL_INT(-2, rc);  /* ENOENT = 2 */
}

void test_unlink_directory_fails(void) {
    uint32_t dino = 0;
    tmpfs_mkdir(TMPFS_ROOT_INO, "mydir", &dino);
    int rc = tmpfs_unlink(TMPFS_ROOT_INO, "mydir");
    TEST_ASSERT_EQUAL_INT(-21, rc);  /* EISDIR = 21; must use rmdir for directories */
}

/* -----------------------------------------------------------------------
 * Tests: TMPFS-04 — mkdir/rmdir
 * --------------------------------------------------------------------- */
void test_mkdir_creates_directory(void) {
    uint32_t dino = 0;
    int rc = tmpfs_mkdir(TMPFS_ROOT_INO, "subdir", &dino);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_EQUAL(0, dino);
    vfs_inode_info_t info;
    TEST_ASSERT_EQUAL_INT(0, tmpfs_lookup("subdir", &info));
    TEST_ASSERT_EQUAL_INT(VFS_FILE_TYPE_DIR, info.file_type);
}

void test_mkdir_then_create_file_inside(void) {
    uint32_t dino = 0;
    tmpfs_mkdir(TMPFS_ROOT_INO, "logs", &dino);
    uint32_t fino = 0;
    int rc = tmpfs_create(dino, "app.log", 0, &fino);
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* Nested path lookup: "logs/app.log" */
    vfs_inode_info_t info;
    int lr = tmpfs_lookup("logs/app.log", &info);
    TEST_ASSERT_EQUAL_INT(0, lr);
    TEST_ASSERT_EQUAL_UINT32(fino, info.inode);
}

void test_rmdir_empty_succeeds(void) {
    uint32_t dino = 0;
    tmpfs_mkdir(TMPFS_ROOT_INO, "empty_dir", &dino);
    int rc = tmpfs_rmdir(TMPFS_ROOT_INO, "empty_dir");
    TEST_ASSERT_EQUAL_INT(0, rc);
    vfs_inode_info_t info;
    TEST_ASSERT_EQUAL_INT(-1, tmpfs_lookup("empty_dir", &info));
}

void test_rmdir_non_empty_fails(void) {
    uint32_t dino = 0;
    tmpfs_mkdir(TMPFS_ROOT_INO, "full_dir", &dino);
    uint32_t fino = 0;
    tmpfs_create(dino, "file.txt", 0, &fino);
    int rc = tmpfs_rmdir(TMPFS_ROOT_INO, "full_dir");
    TEST_ASSERT_EQUAL_INT(-39, rc);   /* ENOTEMPTY = 39 */
}

void test_readdir_root_shows_created_entries(void) {
    uint32_t fino = 0, dino = 0;
    tmpfs_create(TMPFS_ROOT_INO, "file.txt", 0, &fino);
    tmpfs_mkdir(TMPFS_ROOT_INO, "mydir", &dino);
    uint64_t offset = 0;
    tmpfs_readdir(TMPFS_ROOT_INO, &offset, count_cb, NULL);
    /* Root has ".", "..", "file.txt", "mydir" = 4 entries */
    TEST_ASSERT_EQUAL_INT(4, g_readdir_count);
}

int main(void) {
    UNITY_BEGIN();
    /* TMPFS-01 */
    RUN_TEST(test_create_file_in_root);
    RUN_TEST(test_create_file_lookup_succeeds);
    RUN_TEST(test_lookup_nonexistent_fails);
    RUN_TEST(test_create_duplicate_fails);
    /* TMPFS-02 */
    RUN_TEST(test_write_then_read);
    RUN_TEST(test_write_updates_size);
    RUN_TEST(test_read_past_eof_returns_zero);
    RUN_TEST(test_write_across_block_boundary);
    /* TMPFS-03 */
    RUN_TEST(test_unlink_removes_file);
    RUN_TEST(test_unlink_nonexistent_fails);
    RUN_TEST(test_unlink_directory_fails);
    /* TMPFS-04 */
    RUN_TEST(test_mkdir_creates_directory);
    RUN_TEST(test_mkdir_then_create_file_inside);
    RUN_TEST(test_rmdir_empty_succeeds);
    RUN_TEST(test_rmdir_non_empty_fails);
    RUN_TEST(test_readdir_root_shows_created_entries);
    return UNITY_END();
}
