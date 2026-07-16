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
 * testwrite — smoke test for EXT2 write path (Phase 13)
 *
 * Tests:
 *   EXT2W-05: open(O_CREAT|O_WRONLY) returns valid fd
 *   EXT2W-06: write(fd, buf, n) returns n
 *   EXT2W-07: data persists (this binary creates the file; reboot and read with cat)
 */

#include <fcntl.h>
#include <unistd.h>

/* Minimal write_str without printf (avoid newlib stdio for this test) */
static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
}

/* Minimal int-to-decimal string helper */
static void write_int(int v) {
    if (v < 0) { write_str("-"); v = -v; }
    if (v == 0) { write_str("0"); return; }
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write_str(buf + i);
}

int main(void) {
    write_str("[testwrite] Starting EXT2 write test\n");

    /* --- Test 1: Create a new file with O_CREAT|O_WRONLY --- */
    write_str("[testwrite] open(\"/testfile\", O_CREAT|O_WRONLY, 0644)...\n");
    int fd = open("/testfile", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        write_str("[testwrite] FAIL: open returned ");
        write_int(fd);
        write_str("\n");
        return 1;
    }
    write_str("[testwrite] PASS: fd = ");
    write_int(fd);
    write_str("\n");

    /* --- Test 2: Write known content --- */
    const char *msg = "Hello from miniOS ext2 write!\n";
    int msg_len = 0;
    while (msg[msg_len]) msg_len++;

    write_str("[testwrite] write(fd, msg, ");
    write_int(msg_len);
    write_str(")...\n");

    ssize_t n = write(fd, msg, (size_t)msg_len);
    if (n != msg_len) {
        write_str("[testwrite] FAIL: write returned ");
        write_int((int)n);
        write_str("\n");
        close(fd);
        return 1;
    }
    write_str("[testwrite] PASS: wrote ");
    write_int((int)n);
    write_str(" bytes\n");

    /* --- Test 3: Close (triggers flush + ata_flush) --- */
    write_str("[testwrite] close(fd)...\n");
    int r = close(fd);
    if (r != 0) {
        write_str("[testwrite] FAIL: close returned ");
        write_int(r);
        write_str("\n");
        return 1;
    }
    write_str("[testwrite] PASS: close OK (data flushed to disk)\n");

    /* --- Test 4: Read back immediately to verify round-trip --- */
    write_str("[testwrite] Verifying: open(\"/testfile\", O_RDONLY)...\n");
    fd = open("/testfile", 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        write_str("[testwrite] FAIL: re-open for read returned ");
        write_int(fd);
        write_str("\n");
        return 1;
    }

    char rbuf[64];
    ssize_t rn = read(fd, rbuf, sizeof(rbuf) - 1);
    close(fd);

    if (rn <= 0) {
        write_str("[testwrite] FAIL: read back returned ");
        write_int((int)rn);
        write_str("\n");
        return 1;
    }
    rbuf[rn] = '\0';

    /* Verify first byte matches */
    if (rbuf[0] != msg[0]) {
        write_str("[testwrite] FAIL: content mismatch\n");
        return 1;
    }
    write_str("[testwrite] PASS: read back '");
    write(1, rbuf, (size_t)rn);
    write_str("'\n");

    write_str("[testwrite] ALL TESTS PASSED\n");
    write_str("[testwrite] Reboot QEMU and run 'cat /testfile' to verify EXT2W-07\n");
    return 0;
}
