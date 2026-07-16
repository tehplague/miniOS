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

#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>

int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int actions, const struct termios *t) {
    unsigned long request;

    if (actions == TCSANOW) {
        request = TCSETS;
    } else if (actions == TCSADRAIN || actions == TCSAFLUSH) {
        request = TCSETSW;
    } else {
        errno = EINVAL;
        return -1;
    }

    return ioctl(fd, request, (void *)t);
}

void cfmakeraw(struct termios *t) {
    if (!t)
        return;
    t->c_lflag &= ~(ICANON | ECHO | ISIG);
}

int isatty(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        return 0;
    }
    return 1;
}
