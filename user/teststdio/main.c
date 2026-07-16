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
 * teststdio — end-to-end Newlib stdio integration test (Phase 14)
 *
 * Tests:
 *   STDIO-04: printf() produces visible console output
 *   STDIO-05: fopen/fread/fwrite/fclose work end-to-end
 *   STDIO-01: stderr (fd 2) routes to console (via fprintf(stderr,...))
 *   STDIO-03: _open/_close stubs exercise VFS open/close path
 */

#include <stdio.h>
#include <string.h>

int main(void) {
    /* Stage 1: printf (STDIO-04) */
    printf("[teststdio] Stage 1: printf test\n");
    printf("Hello from Newlib printf!\n");
    fprintf(stderr, "[teststdio] stderr also routes to console (STDIO-01)\n");
    printf("[teststdio] Stage 1 PASS\n\n");

    /* Stage 2: fopen("w"), fwrite, fclose (STDIO-05, STDIO-03) */
    const char *testfile = "/stdiotest";
    const char *content  = "Test content from Newlib fwrite\n";
    int clen = (int)strlen(content);

    printf("[teststdio] Stage 2: fopen('w'), fwrite, fclose\n");
    FILE *f = fopen(testfile, "w");   /* Newlib: O_WRONLY|O_CREAT|O_TRUNC */
    if (!f) {
        printf("[teststdio] FAIL: fopen('w') returned NULL\n");
        return 1;
    }
    size_t nw = fwrite(content, 1, (size_t)clen, f);
    fclose(f);
    if ((int)nw != clen) {
        printf("[teststdio] FAIL: fwrite: expected %d, got %zu\n", clen, nw);
        return 1;
    }
    printf("[teststdio] Stage 2 PASS: wrote %d bytes\n\n", clen);

    /* Stage 3: fopen("r"), fread, fclose, compare (STDIO-02, STDIO-05) */
    printf("[teststdio] Stage 3: fopen('r'), fread, fclose, verify\n");
    f = fopen(testfile, "r");
    if (!f) {
        printf("[teststdio] FAIL: fopen('r') returned NULL\n");
        return 1;
    }
    char rbuf[256];
    size_t nr = fread(rbuf, 1, sizeof(rbuf) - 1, f);
    fclose(f);
    rbuf[nr] = '\0';

    if ((int)nr != clen || strcmp(rbuf, content) != 0) {
        printf("[teststdio] FAIL: content mismatch\n");
        printf("  Expected (%d bytes): %s", clen, content);
        printf("  Got      (%zu bytes): %s\n", nr, rbuf);
        return 1;
    }
    printf("[teststdio] Stage 3 PASS: read back %zu bytes, content verified\n\n", nr);

    printf("[teststdio] ALL TESTS PASSED\n");
    return 0;
}
