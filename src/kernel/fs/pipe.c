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

#include <miniOS/fs/pipe.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/mm/heap.h>
#include <string.h>

/**
 * pipe_create() - Allocate and zero-initialise a new pipe object.
 *
 * All fields are zeroed so that write_pos == read_pos == 0 (empty buffer),
 * ref_count == 0 (caller sets it after both fds are established), and
 * write_closed == 0.
 *
 * @return: Pointer to the allocated pipe_t, or NULL on OOM.
 */
pipe_t *pipe_create(void) {
    pipe_t *p = kmalloc(sizeof(pipe_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    return p;
}

/**
 * pipe_read() - Read bytes from the ring buffer.
 * @ino: pipe_t * cast to uint32_t via uintptr_t (vfs_ops_t ABI constraint).
 *       The syscall layer passes (uint32_t)(uintptr_t)vf->pipe as ino.
 * @off: Ignored; pipes have no file offset.
 * @buf: Destination buffer.
 * @len: Maximum bytes to read.
 *
 * Uses monotonic cursor arithmetic: available = write_pos - read_pos.
 * Reads byte-by-byte using read_pos % PIPE_SIZE as the buffer index.
 *
 * @return: Bytes read, or 0 when the buffer is empty (regardless of write_closed).
 */
int pipe_read_buf(pipe_t *p, void *buf, uint32_t len) {
    uint32_t avail = p->write_pos - p->read_pos;
    if (avail == 0) {
        return 0;  /* empty: EOF if write_closed, no data otherwise — both return 0 */
    }
    uint32_t to_read = (len < avail) ? len : avail;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->read_pos % PIPE_SIZE];
        p->read_pos++;
    }
    return (int)to_read;
}

int pipe_read(uint32_t ino, uint64_t off, void *buf, uint32_t len) {
    (void)off;
    return pipe_read_buf((pipe_t *)(uintptr_t)ino, buf, len);
}

/**
 * pipe_write() - Write bytes into the ring buffer.
 * @ino: pipe_t * cast to uint32_t via uintptr_t (vfs_ops_t ABI constraint).
 * @off: Ignored.
 * @buf: Source buffer.
 * @len: Number of bytes to write.
 *
 * Uses monotonic cursor arithmetic: free_space = PIPE_SIZE - (write_pos - read_pos).
 * Writes byte-by-byte using write_pos % PIPE_SIZE as the buffer index.
 * Returns 0 (partial write of 0) when the buffer is completely full.
 *
 * @return: Bytes written (may be less than len if buffer is partially or fully full).
 */
int pipe_write_buf(pipe_t *p, const void *buf, uint32_t len) {
    uint32_t free_space = PIPE_SIZE - (p->write_pos - p->read_pos);
    if (free_space == 0) return 0;  /* buffer full; partial write of 0 */
    uint32_t to_write = (len < free_space) ? len : free_space;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < to_write; i++) {
        p->buf[p->write_pos % PIPE_SIZE] = src[i];
        p->write_pos++;
    }
    return (int)to_write;
}

int pipe_write(uint32_t ino, uint64_t off, const void *buf, uint32_t len) {
    (void)off;
    return pipe_write_buf((pipe_t *)(uintptr_t)ino, buf, len);
}

/**
 * pipe_free() - Release a reference to a pipe; free when ref_count reaches 0.
 * @p: Pipe to release. NULL is a safe no-op.
 *
 * Decrements p->ref_count. If it reaches 0, calls kfree(p) to release the
 * kernel heap allocation.
 */
void pipe_free(pipe_t *p) {
    if (!p) return;
    if (--p->ref_count == 0) kfree(p);
}

/**
 * pipe_ops - VFS ops dispatch table for pipe file descriptors.
 *
 * Only .read and .write are populated; all other ops (lookup, readdir,
 * flush, create, unlink, mkdir, set_root) are NULL because pipes have
 * no filesystem representation and are not seekable or stat-able.
 */
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
