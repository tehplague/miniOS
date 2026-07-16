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

#include <miniOS/drivers/block_device.h>
#include <miniOS/io.h>
#include <string.h>

static block_device_t g_devices[BLKDEV_MAX];
static int g_count;

void blkdev_init(void) {
    memset(g_devices, 0, sizeof(g_devices));
    g_count = 0;
}

int blkdev_register(const char *name, uint8_t major, uint8_t minor,
                    uint64_t lba_start, uint64_t lba_end, bool is_partition) {
    if (g_count >= BLKDEV_MAX) {
        printk("blkdev: registry full, cannot register %s\n", name);
        return -1;
    }
    block_device_t *d = &g_devices[g_count++];
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = '\0';
    d->major        = major;
    d->minor        = minor;
    d->lba_start    = lba_start;
    d->lba_end      = lba_end;
    d->is_partition = is_partition;
    return 0;
}

const block_device_t *blkdev_find(uint8_t major, uint8_t minor) {
    for (int i = 0; i < g_count; i++) {
        if (g_devices[i].major == major && g_devices[i].minor == minor)
            return &g_devices[i];
    }
    return NULL;
}

const block_device_t *blkdev_get(int index) {
    if (index < 0 || index >= g_count)
        return NULL;
    return &g_devices[index];
}

int blkdev_count(void) {
    return g_count;
}
