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

#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char buf[256];
    ssize_t n;

    if (argc > 1) {
        /* File argument supplied: open and read it */
        const char *path = argv[1];
        int fd = open(path, 0);
        if (fd < 0) {
            /* write error to stderr (fd=2) without printf dependency */
            write(2, "cat: cannot open file\n", 22);
            return 1;
        }
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, (size_t)n);
        }
        close(fd);
    } else {
        /* No argument: read from stdin (fd=0) until EOF */
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            write(1, buf, (size_t)n);
        }
    }
    return 0;
}
