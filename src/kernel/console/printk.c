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

#include <miniOS/drivers/console.h>
#include <miniOS/arch/x86_64/spinlock.h>
#include <string.h>
#include <stdarg.h>

/* Serializes concurrent printk calls from multiple CPUs.
   Without this, VGA offset corruption and serial THRE races cause hangs. */
static spinlock_t printk_lock = SPINLOCK_INIT;

__attribute__ ((format (printf, 1, 2))) int printk (const char* format, ...) {
    va_list list;
    va_start (list, format);
    char buf[256];
    char *s = buf;

    int i = vsnprintf(s, 255, format, list);
    va_end (list);

    unsigned long flags;
    spinlock_irqsave(&printk_lock, &flags);
    while (*s != '\0') {
        console_putchar(*s++);
    }
    spinlock_irqrestore(&printk_lock, flags);

    return i;
}
