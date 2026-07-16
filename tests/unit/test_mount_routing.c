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

/* test_mount_routing.c — unit tests for VFS mount routing (MNT-02).
 * Tests vfs_find_mount() longest-prefix-match and boundary-check logic.
 * Directly #includes vfs.c. */

#include "unity.h"
#include <string.h>

#include "../../src/kernel/fs/vfs.c"

int ext2_create(uint32_t p, const char *n, uint16_t m, uint32_t *o) { (void)p;(void)n;(void)m;*o=1;return 0; }
int ext2_truncate_inode(uint32_t i, uint64_t s) { (void)i; (void)s; return 0; }
int ata_flush(void) { return 0; }

static int root_lookup(const char *p, vfs_inode_info_t *o) {
    (void)p; o->inode=2; o->file_type=VFS_FILE_TYPE_DIR; o->size=0; return 0;
}
static int tmp_lookup(const char *p, vfs_inode_info_t *o) {
    (void)p; o->inode=1; o->file_type=VFS_FILE_TYPE_DIR; o->size=0; return 0;
}

static vfs_ops_t root_ops = { root_lookup, NULL, NULL, NULL, NULL };
static vfs_ops_t tmp_ops  = { tmp_lookup,  NULL, NULL, NULL, NULL };

void setUp(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    g_num_mounts = 0;
    vfs_register_mount("/", &root_ops, 0);
    vfs_register_mount("/tmp", &tmp_ops, 0);
}
void tearDown(void) {}

void test_root_path_routes_to_root(void) {
    const char *rel;
    vfs_mount_t *m = vfs_find_mount("/etc/passwd", &rel);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_PTR(&root_ops, m->ops);
    TEST_ASSERT_EQUAL_STRING("etc/passwd", rel);
}

void test_tmp_path_routes_to_tmpfs(void) {
    const char *rel;
    vfs_mount_t *m = vfs_find_mount("/tmp/foo.txt", &rel);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_PTR(&tmp_ops, m->ops);
    TEST_ASSERT_EQUAL_STRING("foo.txt", rel);
}

void test_tmp_exact_routes_to_tmpfs(void) {
    const char *rel;
    vfs_mount_t *m = vfs_find_mount("/tmp", &rel);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_PTR(&tmp_ops, m->ops);
    /* relative path is empty string after stripping "/tmp" */
    TEST_ASSERT_EQUAL_STRING("", rel);
}

void test_tmpfoo_does_not_match_tmp_mount(void) {
    /* "/tmpfoo" must NOT route to /tmp — boundary check */
    const char *rel;
    vfs_mount_t *m = vfs_find_mount("/tmpfoo", &rel);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_PTR(&root_ops, m->ops);  /* falls back to "/" */
}

void test_no_mount_returns_null(void) {
    /* Clear mounts so nothing matches */
    memset(g_mounts, 0, sizeof(g_mounts));
    g_num_mounts = 0;
    const char *rel;
    vfs_mount_t *m = vfs_find_mount("/anything", &rel);
    TEST_ASSERT_NULL(m);
}

void test_nested_path_in_tmp_strips_correctly(void) {
    const char *rel;
    vfs_mount_t *m = vfs_find_mount("/tmp/a/b/c", &rel);
    TEST_ASSERT_EQUAL_PTR(&tmp_ops, m->ops);
    TEST_ASSERT_EQUAL_STRING("a/b/c", rel);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_root_path_routes_to_root);
    RUN_TEST(test_tmp_path_routes_to_tmpfs);
    RUN_TEST(test_tmp_exact_routes_to_tmpfs);
    RUN_TEST(test_tmpfoo_does_not_match_tmp_mount);
    RUN_TEST(test_no_mount_returns_null);
    RUN_TEST(test_nested_path_in_tmp_strips_correctly);
    return UNITY_END();
}
