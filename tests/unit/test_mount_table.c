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

/* test_mount_table.c — unit tests for VFS mount table (MNT-01).
 * Directly #includes vfs.c to access g_mounts[] and g_num_mounts.
 * Linked with: stub_sched.c, stub_keyboard.c, stub_pmm.c, stub_vmm.c */

#include "unity.h"
#include <string.h>

#include "../../src/kernel/fs/vfs.c"

/* Minimal ext2 stubs required because vfs.c has extern ext2_create / ext2_truncate_inode */
int ext2_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino_out) {
    (void)parent_ino; (void)name; (void)mode;
    *new_ino_out = 99;
    return 0;
}
int ext2_truncate_inode(uint32_t ino, uint64_t new_size) { (void)ino; (void)new_size; return 0; }
int ata_flush(void) { return 0; }

static vfs_ops_t fake_ops_a = { NULL, NULL, NULL, NULL, NULL };
static vfs_ops_t fake_ops_b = { NULL, NULL, NULL, NULL, NULL };

void setUp(void) {
    memset(g_mounts, 0, sizeof(g_mounts));
    g_num_mounts = 0;
}
void tearDown(void) {}

void test_register_mount_stores_entry(void) {
    int rc = vfs_register_mount("/", &fake_ops_a, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, g_num_mounts);
    TEST_ASSERT_EQUAL_STRING("/", g_mounts[0].mount_point);
    TEST_ASSERT_EQUAL_PTR(&fake_ops_a, g_mounts[0].ops);
}

void test_register_two_mounts(void) {
    vfs_register_mount("/", &fake_ops_a, 0);
    vfs_register_mount("/tmp", &fake_ops_b, 0);
    TEST_ASSERT_EQUAL_INT(2, g_num_mounts);
    TEST_ASSERT_EQUAL_STRING("/tmp", g_mounts[1].mount_point);
    TEST_ASSERT_EQUAL_PTR(&fake_ops_b, g_mounts[1].ops);
}

void test_register_full_table_returns_error(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        char pt[16];
        pt[0] = '/'; pt[1] = '0' + i; pt[2] = '\0';
        vfs_register_mount(pt, &fake_ops_a, 0);
    }
    int rc = vfs_register_mount("/extra", &fake_ops_a, 0);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_vfs_mount_compat_registers_at_root(void) {
    /* vfs_mount() shim must register at "/" for backwards compat */
    vfs_mount(&fake_ops_a);
    TEST_ASSERT_EQUAL_INT(1, g_num_mounts);
    TEST_ASSERT_EQUAL_STRING("/", g_mounts[0].mount_point);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_register_mount_stores_entry);
    RUN_TEST(test_register_two_mounts);
    RUN_TEST(test_register_full_table_returns_error);
    RUN_TEST(test_vfs_mount_compat_registers_at_root);
    return UNITY_END();
}
