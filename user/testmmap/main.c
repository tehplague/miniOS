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
#include <sys/mman.h>

int main(void) {
    printf("testmmap: starting\n");

    /* Test 1: anonymous read-write mapping */
    void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ptr == MAP_FAILED) {
        printf("FAIL: mmap(PROT_READ|PROT_WRITE) returned MAP_FAILED\n");
        return 1;
    }
    printf("PASS: mmap(NULL, 4096, PROT_READ|PROT_WRITE) = 0x%lx\n",
           (unsigned long)ptr);

    /* Test 2: write to mapped region */
    char *buf = (char *)ptr;
    const char *msg = "Hello, mmap!";
    int msglen = (int)strlen(msg);
    for (int i = 0; i < msglen; i++)
        buf[i] = msg[i];
    buf[msglen] = '\0';
    printf("PASS: wrote '%s' to mapped page\n", buf);

    /* Test 3: read back from mapped region */
    char read_buf[64];
    for (int i = 0; i <= msglen; i++)
        read_buf[i] = buf[i];
    if (read_buf[0] != 'H' || read_buf[5] != ',' ) {
        printf("FAIL: read-back mismatch\n");
        return 1;
    }
    printf("PASS: read back '%s'\n", read_buf);

    /* Test 4: second mapping (8 KiB, read-only) */
    void *ptr2 = mmap(NULL, 8192, PROT_READ,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ptr2 == MAP_FAILED) {
        printf("FAIL: second mmap(PROT_READ) returned MAP_FAILED\n");
        return 1;
    }
    printf("PASS: mmap(NULL, 8192, PROT_READ) = 0x%lx\n",
           (unsigned long)ptr2);

    /* Verify second mapping is zero-filled */
    const char *p2 = (const char *)ptr2;
    if (p2[0] != 0 || p2[4095] != 0) {
        printf("FAIL: second mapping not zero-filled\n");
        return 1;
    }
    printf("PASS: second mapping is zero-filled\n");

    /* Test 5: munmap first region */
    int r = munmap(ptr, 4096);
    if (r != 0) {
        printf("FAIL: munmap(ptr, 4096) returned %d\n", r);
        return 1;
    }
    printf("PASS: munmap(0x%lx, 4096) = 0\n", (unsigned long)ptr);

    /* Test 6: munmap second region */
    r = munmap(ptr2, 8192);
    if (r != 0) {
        printf("FAIL: munmap(ptr2, 8192) returned %d\n", r);
        return 1;
    }
    printf("PASS: munmap(0x%lx, 8192) = 0\n", (unsigned long)ptr2);

    printf("testmmap: all tests passed\n");
    return 0;
}
