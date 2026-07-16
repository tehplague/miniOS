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

/* stub_pipe.c — no-op pipe stubs for tests that include vfs.c (which calls
 * pipe_free on VFS_FILE_TYPE_PIPE close) but do not test pipe functionality.
 * Used by test_vfs, test_mount_table, test_mount_routing. */

#include <miniOS/fs/vfs.h>

pipe_t *pipe_create(void) { return NULL; }
int pipe_read(uint32_t ino, uint64_t off, void *buf, uint32_t len) {
    (void)ino; (void)off; (void)buf; (void)len; return 0;
}
int pipe_write(uint32_t ino, uint64_t off, const void *buf, uint32_t len) {
    (void)ino; (void)off; (void)buf; (void)len; return 0;
}
void pipe_free(pipe_t *p) { (void)p; }

struct vfs_ops pipe_ops = {
    .lookup   = NULL,
    .read     = pipe_read,
    .readdir  = NULL,
    .write    = pipe_write,
    .flush    = NULL,
    .create   = NULL,
    .unlink   = NULL,
    .mkdir    = NULL,
    .set_root = NULL,
};
