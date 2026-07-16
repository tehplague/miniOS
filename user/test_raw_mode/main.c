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
#include <termios.h>
#include <unistd.h>

int main() {
    struct termios oldt, newt;
    char c;

    // Get current terminal settings
    if (tcgetattr(STDIN_FILENO, &oldt) < 0) {
        perror("tcgetattr");
        return 1;
    }

    newt = oldt;
    cfmakeraw(&newt);

    // Set raw mode
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0) {
        perror("tcsetattr");
        return 1;
    }

    printf("In raw mode. Press any key.\n");

    // Read a single character
    if (read(STDIN_FILENO, &c, 1) < 0) {
        perror("read");
        return 1;
    }

    printf("Read char: 0x%02x\n", (unsigned char)c);

    // Restore old settings
    if (tcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0) {
        perror("tcsetattr");
        return 1;
    }

    return 0;
}
