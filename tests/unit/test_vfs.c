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

/* test_vfs.c — VFS dispatch unit tests.
 * Directly #includes vfs.c to access static mount table.
 * Linked with: stub_sched.c (provides sched_current + struct thread). */

#include "unity.h"
#include <string.h>

extern char stub_tty_read_char;
extern char stub_keyboard_char;
extern char stub_tty_write_buf[32];
extern uint32_t stub_tty_write_len;
extern uint32_t stub_tty_read_calls;
extern uint32_t stub_tty_write_calls;

/* Pull in VFS implementation directly to access static g_mounts */
#include "../../src/kernel/fs/vfs.c"

/* Minimal stubs for symbols referenced by vfs.c */
int ext2_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino_out) {
    (void)parent_ino; (void)name; (void)mode;
    *new_ino_out = 99;
    return 0;
}
int ext2_truncate_inode(uint32_t ino, uint64_t new_size) { (void)ino; (void)new_size; return 0; }
int ata_flush(void) { return 0; }

/* -----------------------------------------------------------------------
 * Fake VFS ops table for testing
 * --------------------------------------------------------------------- */

static uint64_t g_last_readdir_offset;
static uint32_t g_last_unlink_parent_ino;
static char g_last_unlink_name[64];
static uint32_t g_last_rmdir_parent_ino;
static char g_last_rmdir_name[64];

/* test_lookup: fills inode metadata for known fake paths.
 * With the mount table, vfs_find_mount("/") strips the leading "/" so
 * paths arrive as "test" and "dir" (without leading slash), EXCEPT the
 * root "/" itself which arrives as "" (empty string for the root dir). */
static int test_lookup(const char *path, vfs_inode_info_t *out) {
    if (!path || !out)
        return -1;

    /* Root directory: empty relative path or "/" */
    if (strcmp(path, "") == 0 || strcmp(path, "/") == 0) {
        out->inode = 1;
        out->file_type = VFS_FILE_TYPE_DIR;
        out->device_type = VFS_DEVICE_NONE;
        out->size = 64;
        return 0;
    }

    if (strcmp(path, "test") == 0) {
        out->inode = 42;
        out->file_type = VFS_FILE_TYPE_REG;
        out->device_type = VFS_DEVICE_NONE;
        out->size = 5;
        return 0;
    }

    if (strcmp(path, "dir") == 0) {
        out->inode = 77;
        out->file_type = VFS_FILE_TYPE_DIR;
        out->device_type = VFS_DEVICE_NONE;
        out->size = 128;
        return 0;
    }

    if (strcmp(path, "tty") == 0) {
        out->inode = 88;
        out->file_type = VFS_FILE_TYPE_CHAR;
        out->device_type = VFS_DEVICE_TTY;
        out->size = 0;
        return 0;
    }

    if (strcmp(path, "null") == 0) {
        out->inode = 89;
        out->file_type = VFS_FILE_TYPE_CHAR;
        out->device_type = VFS_DEVICE_NULL;
        out->size = 0;
        return 0;
    }

    return -1;
}

/* test_read: writes "hello" into buf, returns 5 */
static int test_read(uint32_t ino, uint64_t off, void *buf, uint32_t len) {
    (void)ino; (void)off;
    const char *data = "hello";
    uint32_t n = (len < 5) ? len : 5;
    for (uint32_t i = 0; i < n; i++) ((char *)buf)[i] = data[i];
    return (int)n;
}

/* test_readdir: records and advances the caller-owned directory offset */
static int test_readdir(uint32_t ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud) {
    (void)ino;
    (void)cb;
    (void)ud;

    if (!offset)
        return -1;

    g_last_readdir_offset = *offset;
    *offset += 24;
    return 0;
}

static int test_unlink_op(uint32_t parent_ino, const char *name) {
    g_last_unlink_parent_ino = parent_ino;
    strncpy(g_last_unlink_name, name, sizeof(g_last_unlink_name) - 1);
    g_last_unlink_name[sizeof(g_last_unlink_name) - 1] = '\0';
    return 0;
}

static int test_rmdir_op(uint32_t parent_ino, const char *name) {
    g_last_rmdir_parent_ino = parent_ino;
    strncpy(g_last_rmdir_name, name, sizeof(g_last_rmdir_name) - 1);
    g_last_rmdir_name[sizeof(g_last_rmdir_name) - 1] = '\0';
    return 0;
}

static vfs_ops_t fake_ops = {
    .lookup  = test_lookup,
    .read    = test_read,
    .readdir = test_readdir,
    .unlink  = test_unlink_op,
    .rmdir   = test_rmdir_op,
};

/* -----------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------- */
void setUp(void) {
    /* Zero out the test thread's fd_table via sched_current() */
    struct thread *t = sched_current();
    memset(t->fd_table, 0, VFS_MAX_FDS * sizeof(*t->fd_table));
    /* Simulate stdin/stdout/stderr so alloc_fd() starts at VFS_FIRST_OPEN_FD */
    for (int i = 0; i < VFS_FIRST_OPEN_FD; i++)
        t->fd_table[i].in_use = 1;
    /* Initialize test thread cwd to "/" so vfs_open relative path tests work */
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    /* Reset mount table */
    memset(g_mounts, 0, sizeof(g_mounts));
    g_num_mounts = 0;
    g_last_readdir_offset = 0;
    g_last_unlink_parent_ino = 0;
    g_last_unlink_name[0] = '\0';
    g_last_rmdir_parent_ino = 0;
    g_last_rmdir_name[0] = '\0';
    stub_tty_read_char = 'T';
    stub_keyboard_char = 'T';
    memset(stub_tty_write_buf, 0, sizeof(stub_tty_write_buf));
    stub_tty_write_len = 0;
    stub_tty_read_calls = 0;
    stub_tty_write_calls = 0;
}

void tearDown(void) {
    /* nothing */
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/* vfs_open returns -1 when no filesystem is mounted */
void test_open_returns_minus1_when_no_fs_mounted(void) {
    int fd = vfs_open("/test", 0, 0);
    TEST_ASSERT_EQUAL_INT(-1, fd);
}

/* vfs_mount + vfs_open with a fake ops returns fd >= 0 */
void test_open_returns_valid_fd_after_mount(void) {
    vfs_mount(&fake_ops);
    int fd = vfs_open("/test", 0, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(VFS_FIRST_OPEN_FD, fd);
}

/* vfs_open returns -1 when lookup returns 0 (file not found) */
void test_open_returns_minus1_when_file_not_found(void) {
    vfs_mount(&fake_ops);
    int fd = vfs_open("/nonexistent", 0, 0);
    TEST_ASSERT_EQUAL_INT(-1, fd);
}

/* vfs_read on opened fd calls ops->read and advances offset */
void test_read_calls_ops_and_advances_offset(void) {
    vfs_mount(&fake_ops);
    int fd = vfs_open("/test", 0, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(VFS_FIRST_OPEN_FD, fd);

    char buf[16] = {0};
    int n = vfs_read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_CHAR('h', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('e', buf[1]);

    /* Verify offset advanced: check fd_table.offset == 5 */
    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_UINT64(5, t->fd_table[fd].offset);
}

/* vfs_close marks fd as not in_use; subsequent vfs_read returns -1 */
void test_close_marks_fd_as_not_in_use(void) {
    vfs_mount(&fake_ops);
    int fd = vfs_open("/test", 0, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(VFS_FIRST_OPEN_FD, fd);

    int r = vfs_close(fd);
    TEST_ASSERT_EQUAL_INT(0, r);

    /* fd_table[fd].in_use should now be 0 */
    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_UINT8(0, t->fd_table[fd].in_use);

    /* vfs_read on closed fd should return -1 */
    char buf[8];
    int n = vfs_read(fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* opening VFS_MAX_FDS files exhausts fd table; next open returns -1 */
void test_fd_table_exhaustion_returns_minus1(void) {
    vfs_mount(&fake_ops);
    for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
        int fd = vfs_open("/test", 0, 0);
        TEST_ASSERT_GREATER_OR_EQUAL(VFS_FIRST_OPEN_FD, fd);
    }
    /* One more open should fail */
    int fd = vfs_open("/test", 0, 0);
    TEST_ASSERT_EQUAL_INT(-1, fd);
}

void test_open_skips_stdio_fds(void) {
    vfs_mount(&fake_ops);
    TEST_ASSERT_EQUAL_INT(VFS_FIRST_OPEN_FD, vfs_open("/test", 0, 0));
}

void test_read_rejects_directory_fd(void) {
    vfs_mount(&fake_ops);

    int fd = vfs_open("/dir", 0, 0);
    TEST_ASSERT_EQUAL_INT(VFS_FIRST_OPEN_FD, fd);

    char buf[8];
    TEST_ASSERT_EQUAL_INT(-1, vfs_read(fd, buf, sizeof(buf)));
}

void test_readdir_advances_directory_offset(void) {
    vfs_mount(&fake_ops);

    int fd = vfs_open("/dir", 0, 0);
    TEST_ASSERT_EQUAL_INT(VFS_FIRST_OPEN_FD, fd);

    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_UINT64(0, t->fd_table[fd].offset);

    TEST_ASSERT_EQUAL_INT(0, vfs_readdir(fd, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, g_last_readdir_offset);
    TEST_ASSERT_EQUAL_UINT64(24, t->fd_table[fd].offset);

    TEST_ASSERT_EQUAL_INT(0, vfs_readdir(fd, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(24, g_last_readdir_offset);
    TEST_ASSERT_EQUAL_UINT64(48, t->fd_table[fd].offset);
}

void test_char_device_tty_routes_reads_and_writes(void) {
    vfs_mount(&fake_ops);

    int fd = vfs_open("/tty", 0, 0);
    TEST_ASSERT_EQUAL_INT(VFS_FIRST_OPEN_FD, fd);

    char c = 0;
    TEST_ASSERT_EQUAL_INT(1, vfs_read(fd, &c, 1));
    TEST_ASSERT_EQUAL_CHAR('T', c);
    TEST_ASSERT_EQUAL_UINT32(1, stub_tty_read_calls);

    TEST_ASSERT_EQUAL_INT(3, vfs_write(fd, "abc", 3));
    TEST_ASSERT_EQUAL_UINT32(1, stub_tty_write_calls);
    TEST_ASSERT_EQUAL_UINT32(3, stub_tty_write_len);
    TEST_ASSERT_EQUAL_MEMORY("abc", stub_tty_write_buf, 3);
}

void test_char_device_null_returns_eof_and_discards_writes(void) {
    vfs_mount(&fake_ops);

    int fd = vfs_open("/null", 0, 0);
    TEST_ASSERT_EQUAL_INT(VFS_FIRST_OPEN_FD, fd);

    char c = 'X';
    TEST_ASSERT_EQUAL_INT(0, vfs_read(fd, &c, 1));
    TEST_ASSERT_EQUAL_CHAR('X', c);
    TEST_ASSERT_EQUAL_INT(4, vfs_write(fd, "drop", 4));
}

/* vfs_read with invalid fd (negative) returns -1 */
void test_read_with_negative_fd_returns_minus1(void) {
    int n = vfs_read(-1, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* vfs_read with fd >= VFS_MAX_FDS returns -1 */
void test_read_with_out_of_range_fd_returns_minus1(void) {
    int n = vfs_read(VFS_MAX_FDS, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* vfs_open with a relative path prepends cwd to form the full path.
 * cwd="/", open("test") -> resolves to "/test" -> mount strips "/" -> lookup("test")
 * test_lookup already handles "test" (file, inode 42). */
void test_vfs_relative_path_prepends_cwd(void) {
    vfs_mount(&fake_ops);

    /* cwd is already set to "/" by setUp() */
    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_CHAR('/', t->cwd[0]);

    /* "test" is a relative path; vfs_open should prepend cwd "/" -> "/test"
     * vfs_find_mount("/test") matches "/" -> relative_path="test"
     * test_lookup("test") returns VFS_FILE_TYPE_REG, inode 42 */
    int fd = vfs_open("test", 0, 0);

    /* Should succeed: fd >= VFS_FIRST_OPEN_FD means the path was resolved correctly */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(VFS_FIRST_OPEN_FD, fd);

    /* Clean up */
    vfs_close(fd);
}

/* -----------------------------------------------------------------------
 * Phase 32 symlink tests
 * --------------------------------------------------------------------- */

/* lookup that returns a symlink inode for "/somelink" */
static int symlink_lookup(const char *path, vfs_inode_info_t *out) {
    if (!path || !out) return -1;
    if (strcmp(path, "") == 0 || strcmp(path, "/") == 0) {
        out->inode = 1; out->file_type = VFS_FILE_TYPE_DIR;
        out->device_type = VFS_DEVICE_NONE; out->size = 64;
        return 0;
    }
    if (strcmp(path, "somelink") == 0) {
        out->inode = 10; out->file_type = VFS_FILE_TYPE_SYMLINK;
        out->device_type = VFS_DEVICE_NONE; out->size = 6;
        return 0;
    }
    if (strcmp(path, "regular") == 0) {
        out->inode = 20; out->file_type = VFS_FILE_TYPE_REG;
        out->device_type = VFS_DEVICE_NONE; out->size = 42;
        return 0;
    }
    return -1;
}

static int symlink_readlink_fn(uint32_t ino, char *buf, uint32_t len) {
    (void)ino;
    const char *target = "myfile";
    uint32_t tlen = 6;
    uint32_t copy = (tlen < len) ? tlen : len;
    for (uint32_t i = 0; i < copy; i++) buf[i] = target[i];
    return (int)copy;
}

static vfs_ops_t symlink_ops = {
    .lookup   = symlink_lookup,
    .read     = test_read,
    .readdir  = test_readdir,
    .readlink = symlink_readlink_fn,
};

/* vfs_lstat on a symlink path must return VFS_FILE_TYPE_SYMLINK (no following) */
void test_vfs_lstat_returns_symlink_type(void) {
    vfs_mount(&symlink_ops);
    vfs_inode_info_t out = {0};
    int r = vfs_lstat("/somelink", &out);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILE_TYPE_SYMLINK, out.file_type);
}

/* vfs_readlink on a non-symlink path must return -22 (EINVAL) */
void test_vfs_readlink_nonlink_returns_einval(void) {
    vfs_mount(&symlink_ops);
    char buf[64] = {0};
    int r = vfs_readlink("/regular", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-22, r);
}

/* vfs_readlink on a symlink returns the raw target bytes */
void test_vfs_readlink_returns_target(void) {
    vfs_mount(&symlink_ops);
    char buf[64] = {0};
    int r = vfs_readlink("/somelink", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(6, r);
    TEST_ASSERT_EQUAL_CHAR('m', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('y', buf[1]);
    TEST_ASSERT_EQUAL_CHAR('f', buf[2]);
}

void test_vfs_unlink_delegates_to_filesystem_op(void) {
    vfs_mount(&fake_ops);
    TEST_ASSERT_EQUAL_INT(0, vfs_unlink("/test"));
    TEST_ASSERT_EQUAL_UINT32(1, g_last_unlink_parent_ino);
    TEST_ASSERT_EQUAL_STRING("test", g_last_unlink_name);
}

void test_vfs_rmdir_delegates_to_filesystem_op(void) {
    vfs_mount(&fake_ops);
    TEST_ASSERT_EQUAL_INT(0, vfs_rmdir("/dir"));
    TEST_ASSERT_EQUAL_UINT32(1, g_last_rmdir_parent_ino);
    TEST_ASSERT_EQUAL_STRING("dir", g_last_rmdir_name);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_open_returns_minus1_when_no_fs_mounted);
    RUN_TEST(test_open_returns_valid_fd_after_mount);
    RUN_TEST(test_open_returns_minus1_when_file_not_found);
    RUN_TEST(test_read_calls_ops_and_advances_offset);
    RUN_TEST(test_close_marks_fd_as_not_in_use);
    RUN_TEST(test_fd_table_exhaustion_returns_minus1);
    RUN_TEST(test_open_skips_stdio_fds);
    RUN_TEST(test_read_rejects_directory_fd);
    RUN_TEST(test_readdir_advances_directory_offset);
    RUN_TEST(test_char_device_tty_routes_reads_and_writes);
    RUN_TEST(test_char_device_null_returns_eof_and_discards_writes);
    RUN_TEST(test_read_with_negative_fd_returns_minus1);
    RUN_TEST(test_read_with_out_of_range_fd_returns_minus1);
    RUN_TEST(test_vfs_relative_path_prepends_cwd);
    RUN_TEST(test_vfs_lstat_returns_symlink_type);
    RUN_TEST(test_vfs_readlink_nonlink_returns_einval);
    RUN_TEST(test_vfs_readlink_returns_target);
    RUN_TEST(test_vfs_unlink_delegates_to_filesystem_op);
    RUN_TEST(test_vfs_rmdir_delegates_to_filesystem_op);
    return UNITY_END();
}
