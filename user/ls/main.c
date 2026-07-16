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

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

/* linux_dirent64-compatible struct as filled by SYS_getdents (kernel case 78).
 * d_type values: DT_UNKNOWN=0, DT_DIR=4, DT_REG=8. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];   /* oversized for stack layout; actual name is NUL-terminated */
};

#define DT_DIR 4
#define DT_REG 8

static long getdents(int fd, void *buf, unsigned int count) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(78L), "D"((long)fd), "S"(buf), "d"((long)count)
        : "rcx", "r11", "memory");
    return ret;
}


/* Print permissions string for long format.
 * Phase 17: fixed defaults per type (no per-inode mode lookup).
 * Directories: drwxr-xr-x
 * Regular files: -rw-r--r--
 */
static void print_perms(uint8_t d_type) {
    if (d_type == DT_DIR) {
        printf("drwxr-xr-x");
    } else {
        printf("-rw-r--r--");
    }
}

int main(int argc, char **argv) {
    const char *path = "/";   /* default directory */
    int long_format = 0;

    /* Parse arguments: accept -l flag and optional path */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-l", 2) == 0) {
            long_format = 1;
        } else {
            path = argv[i];   /* last positional arg is the path */
        }
    }

    printf("ls looking up path: %s\n", path);

    int fd = open(path, 0);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    /* Buffer for getdents entries — 4 KiB is sufficient for small directories */
    char buf[4096];
    long n;

    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        long pos = 0;
        while (pos < n) {
            struct linux_dirent64 *ent = (struct linux_dirent64 *)(buf + pos);

            /* Skip . and .. entries */
            if (!(ent->d_name[0] == '.' &&
                  (ent->d_name[1] == '\0' ||
                   (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))) {

                if (long_format) {
                    /* Long format: "drwxr-xr-x     size name" */
                    print_perms(ent->d_type);
                    long size = 0;
                    if (ent->d_type != DT_DIR) {
                        /* Build full path: path + "/" + name */
                        char fullpath[512];
                        int plen = (int)strlen(path);
                        int nlen = (int)strlen(ent->d_name);
                        int need_slash = (plen > 0 && path[plen - 1] != '/') ? 1 : 0;
                        if (plen + need_slash + nlen < (int)sizeof(fullpath) - 1) {
                            memcpy(fullpath, path, (size_t)plen);
                            if (need_slash) fullpath[plen++] = '/';
                            memcpy(fullpath + plen, ent->d_name, (size_t)nlen + 1);
                            int efd = open(fullpath, 0);
                            if (efd >= 0) {
                                size = (long)lseek(efd, 0, SEEK_END);
                                if (size < 0) size = 0;
                                close(efd);
                            }
                        }
                    }
                    printf(" %8ld %s\n", size, ent->d_name);
                } else {
                    /* Simple format: append "/" for directories */
                    if (ent->d_type == DT_DIR) {
                        printf("%s/\n", ent->d_name);
                    } else {
                        printf("%s\n", ent->d_name);
                    }
                }
            }

            pos += ent->d_reclen;
        }
    }

    close(fd);
    return 0;
}
