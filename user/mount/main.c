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

#include <unistd.h>

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

static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(2, s, (size_t)n);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        write_str("Usage: mount <type> <target>\\n");
        return 1;
    }
    const char *fstype = argv[1];
    const char *target = argv[2];

    int r = mount(NULL, target, fstype, 0, NULL);
    if (r < 0) {
        write_str("mount: failed\\n");
        return 1;
    }
    return 0;
}
