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

/*
 * testmountstat — integration test for Phase 20 syscalls
 *
 * Tests:
 *   MNT-04: SYS_mount("tmpfs", "/mnt", "tmpfs", 0, NULL) succeeds (returns 0)
 *   MNT-05: SYS_umount("/mnt", 0) returns 0 (stub)
 *   STAT-01: stat("/tmp/stattest.txt") returns correct size, non-zero inode, S_IFREG mode
 *   STAT-02: fstat(fd) returns same inode and size as stat() on the same file
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int mount(const char *source, const char *target, const char *fstype,
                 unsigned long flags, const void *data) {
    long ret;
    register long r10 __asm__("r10") = (long)flags;
    register long r8  __asm__("r8")  = (long)data;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(165L), "D"(source), "S"(target), "d"(fstype), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)ret;
}
static int umount(const char *target) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(166L), "D"(target)
        : "rcx", "r11", "memory");
    return (int)ret;
}

static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
}

static void write_uint(unsigned long v) {
    if (v == 0) { write_str("0"); return; }
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write_str(buf + i);
}

static void write_oct(unsigned long v) {
    if (v == 0) { write_str("0"); return; }
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 8); v /= 8; }
    write_str("0");  /* octal prefix */
    write_str(buf + i);
}

static int g_failures = 0;

static void check(const char *label, int cond) {
    write_str("[testmountstat] ");
    write_str(cond ? "PASS" : "FAIL");
    write_str(": ");
    write_str(label);
    write_str("\n");
    if (!cond) g_failures++;
}

int main(void) {
    write_str("[testmountstat] Starting Phase 20 syscall integration test\n");

    /* ---------------------------------------------------------------
     * MNT-04: mount tmpfs at /mnt
     * --------------------------------------------------------------- */
    write_str("[testmountstat] Test MNT-04: SYS_mount tmpfs at /mnt\n");
    int r = mount(NULL, "/mnt", "tmpfs", 0, NULL);
    check("MNT-04: mount(NULL, \"/mnt\", \"tmpfs\", 0, NULL) == 0", r == 0);

    /* Create a file under /mnt to verify the mount is live */
    int mfd = open("/mnt/hello.txt", O_CREAT | O_WRONLY, 0644);
    check("MNT-04: can create file under /mnt after mount", mfd >= 0);
    if (mfd >= 0) {
        write(mfd, "hi", 2);
        close(mfd);
    }

    /* ---------------------------------------------------------------
     * MNT-05: umount /mnt (stub — just returns 0)
     * --------------------------------------------------------------- */
    write_str("[testmountstat] Test MNT-05: SYS_umount /mnt\n");
    r = umount("/mnt");
    check("MNT-05: umount(\"/mnt\") == 0", r == 0);

    /* ---------------------------------------------------------------
     * STAT-01: stat on a tmpfs file
     * Create /tmp/stattest.txt with known content, then stat it
     * --------------------------------------------------------------- */
    write_str("[testmountstat] Test STAT-01: stat(\"/tmp/stattest.txt\")\n");

    /* Create the test file with known 13-byte content */
    int sfd = open("/tmp/stattest.txt", O_CREAT | O_WRONLY, 0644);
    check("STAT-01: open O_CREAT /tmp/stattest.txt succeeds", sfd >= 0);
    if (sfd >= 0) {
        ssize_t n = write(sfd, "hello, world!", 13);
        check("STAT-01: write 13 bytes succeeds", n == 13);
        close(sfd);
    }

    struct stat st;
    r = stat("/tmp/stattest.txt", &st);
    check("STAT-01: stat() returns 0", r == 0);

    if (r == 0) {
        write_str("[testmountstat] STAT-01: st_size=");
        write_uint((unsigned long)st.st_size);
        write_str(" st_ino=");
        write_uint((unsigned long)st.st_ino);
        write_str(" st_mode=0");
        write_oct((unsigned long)st.st_mode);
        write_str("\n");

        check("STAT-01: st_size == 13", st.st_size == 13);
        check("STAT-01: st_ino != 0 (valid inode)", st.st_ino != 0);
        /* S_IFREG = 0100000; 0100000 | 0644 = 0100644 */
        check("STAT-01: st_mode has S_IFREG bit (0100000)", (st.st_mode & 0170000) == 0100000);
    }

    /* ---------------------------------------------------------------
     * STAT-02: fstat on same file — must match stat results
     * --------------------------------------------------------------- */
    write_str("[testmountstat] Test STAT-02: fstat fd matches stat\n");

    int fd2 = open("/tmp/stattest.txt", O_RDONLY, 0);
    check("STAT-02: open /tmp/stattest.txt for fstat succeeds", fd2 >= 0);

    if (fd2 >= 0) {
        struct stat st2;
        r = fstat(fd2, &st2);
        check("STAT-02: fstat() returns 0", r == 0);

        if (r == 0) {
            write_str("[testmountstat] STAT-02: fst_size=");
            write_uint((unsigned long)st2.st_size);
            write_str(" fst_ino=");
            write_uint((unsigned long)st2.st_ino);
            write_str("\n");

            check("STAT-02: fstat st_size == stat st_size", st2.st_size == st.st_size);
            check("STAT-02: fstat st_ino == stat st_ino", st2.st_ino == st.st_ino);
            check("STAT-02: fstat st_mode == stat st_mode", st2.st_mode == st.st_mode);
        }
        close(fd2);
    }

    /* ---------------------------------------------------------------
     * Summary
     * --------------------------------------------------------------- */
    if (g_failures == 0) {
        write_str("[testmountstat] ALL TESTS PASSED\n");
    } else {
        write_str("[testmountstat] SOME TESTS FAILED (failures=");
        write_uint((unsigned long)g_failures);
        write_str(")\n");
    }

    return g_failures;
}
