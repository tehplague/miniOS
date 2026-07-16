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
#include <sys/mman.h>

int main(void) {
    printf("testmmap_file: starting\n");

    /* Open a known small file on the root filesystem.
     * /test/bin/hello is always present on the disk image and small enough to fit in one page.
     * Use it as the reference file: its content is deterministic. */
    const char *testfile = "/test/bin/hello";
    int fd = open(testfile, O_RDONLY);
    if (fd < 0) {
        printf("FAIL: open(%s) failed\n", testfile);
        return 1;
    }
    printf("PASS: open(%s) succeeded (fd=%d)\n", testfile, fd);

    /* Read file content via SYS_read to get the reference bytes */
    char file_content[4096];
    int bytes_via_read = (int)read(fd, file_content, sizeof(file_content));
    if (bytes_via_read <= 0) {
        printf("FAIL: read(%s) failed or empty (%d bytes)\n", testfile, bytes_via_read);
        close(fd);
        return 1;
    }
    printf("PASS: read(%s) succeeded (%d bytes)\n", testfile, bytes_via_read);

    /* Reset fd offset to 0 so mmap reads from the start */
    lseek(fd, 0, SEEK_SET);

    /* Map the file into memory (read-only, private, offset=0) */
    void *mapped = mmap(NULL, (size_t)bytes_via_read, PROT_READ,
                        MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL: mmap() returned MAP_FAILED\n");
        close(fd);
        return 1;
    }
    printf("PASS: mmap() succeeded (mapped at 0x%lx)\n", (unsigned long)mapped);

    /* Compare mapped bytes to bytes read via SYS_read */
    if (memcmp(mapped, file_content, (size_t)bytes_via_read) != 0) {
        printf("FAIL: mapped content does not match read content\n");
        munmap(mapped, (size_t)bytes_via_read);
        close(fd);
        return 1;
    }
    printf("PASS: mapped content matches read content (%d bytes)\n", bytes_via_read);

    /* Unmap the region */
    int r = munmap(mapped, (size_t)bytes_via_read);
    if (r != 0) {
        printf("FAIL: munmap() returned %d\n", r);
        close(fd);
        return 1;
    }
    printf("PASS: munmap() succeeded\n");

    close(fd);
    printf("testmmap_file: all tests passed\n");
    return 0;
}
