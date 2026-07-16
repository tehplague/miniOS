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

#ifndef _MINIOS_FS_PIPE_H_
#define _MINIOS_FS_PIPE_H_

#include <miniOS/types.h>

/* Forward-declare the ops struct to break the circular include with vfs.h.
 * vfs.h includes pipe.h (pipe_t in vfs_file_t); pipe.h uses struct vfs_ops *
 * to avoid needing the full vfs_ops_t typedef. The typedef and full definition
 * live in vfs.h which callers should include directly. */
struct vfs_ops;

/**
 * PIPE_SIZE - Capacity of the ring-buffer backing each anonymous pipe.
 * Must be a power of 2 so that monotonic uint32_t cursors can index with
 * a simple modulo (write_pos % PIPE_SIZE).
 */
#define PIPE_SIZE 4096

/**
 * pipe_t - Kernel object for an anonymous pipe.
 *
 * Uses monotonically increasing uint32_t cursors (never reset). The actual
 * buffer index is cursor % PIPE_SIZE. This eliminates the full/empty
 * ambiguity that plagues same-value-cursor designs.
 *
 * Available bytes = write_pos - read_pos
 * Free space      = PIPE_SIZE - (write_pos - read_pos)
 *
 * uint32_t overflow at 2^32 wraps cleanly because PIPE_SIZE is a power of 2
 * and all arithmetic is unsigned modulo 2^32.
 */
typedef struct {
    uint8_t  buf[PIPE_SIZE];
    uint32_t write_pos;    /* monotonically increasing; use % PIPE_SIZE to index */
    uint32_t read_pos;     /* monotonically increasing; use % PIPE_SIZE to index */
    uint32_t ref_count;    /* total number of fds (read + write ends) alive */
    uint8_t  write_closed; /* non-zero when all write-end fds have been closed */
} pipe_t;

/**
 * pipe_create() - Allocate and initialise a new pipe object.
 *
 * Uses kmalloc to allocate the pipe_t; zero-fills all fields so that
 * write_pos==read_pos==ref_count==write_closed==0.
 *
 * @return: Pointer to the new pipe_t on success, NULL if kmalloc returns NULL.
 */
pipe_t *pipe_create(void);

/**
 * pipe_read() - Read bytes from a pipe ring buffer.
 * @ino: pipe_t pointer cast to uint32_t via uintptr_t (vfs_ops_t constraint).
 * @off: Ignored (pipes have no seek position).
 * @buf: Destination buffer for read bytes.
 * @len: Maximum number of bytes to read.
 *
 * Reads up to min(len, available) bytes. Advances read_pos.
 * If available == 0, returns 0 regardless of write_closed state.
 *
 * @return: Number of bytes read, or 0 if the buffer is empty.
 */
int pipe_read(uint32_t ino, uint64_t off, void *buf, uint32_t len);

/**
 * pipe_write() - Write bytes into a pipe ring buffer.
 * @ino: pipe_t pointer cast to uint32_t via uintptr_t (vfs_ops_t constraint).
 * @off: Ignored (pipes have no seek position).
 * @buf: Source buffer for bytes to write.
 * @len: Number of bytes to write.
 *
 * Writes up to min(len, free_space) bytes. Advances write_pos.
 * If the buffer is full, returns 0 (partial write of 0 bytes).
 *
 * @return: Number of bytes written (may be less than len if buffer fills).
 */
int pipe_write(uint32_t ino, uint64_t off, const void *buf, uint32_t len);

/**
 * pipe_free() - Decrement reference count and free when it reaches zero.
 * @p: Pipe to release. If NULL, this is a safe no-op.
 *
 * Decrements p->ref_count. Calls kfree(p) only when ref_count reaches 0.
 */
void pipe_free(pipe_t *p);

/** pipe_ops - VFS ops dispatch table for pipe file descriptors.
 * Declared as struct vfs_ops (not vfs_ops_t typedef) to avoid circular
 * dependency with vfs.h. Callers that include vfs.h can use the typedef. */
extern struct vfs_ops pipe_ops;

/**
 * pipe_write_buf() / pipe_read_buf() — direct pointer variants.
 *
 * These bypass the vfs_ops_t ino-as-uint32_t ABI, which cannot represent
 * kernel-heap pointers above 4 GB.  Use these in the kernel fast-paths
 * (e.g. syscall_dispatch) where the pipe_t * is already available.
 */
int pipe_write_buf(pipe_t *p, const void *buf, uint32_t len);
int pipe_read_buf(pipe_t *p, void *buf, uint32_t len);

#endif /* _MINIOS_FS_PIPE_H_ */
