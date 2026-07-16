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

#ifndef _MINIOS_FS_SYSFS_H_
#define _MINIOS_FS_SYSFS_H_

#include <miniOS/fs/vfs.h>

/* Node types */
#define SYSFS_NODE_DIR  1
#define SYSFS_NODE_FILE 2

/* Static content buffer size — covers "0xNNNNNNNN\n" (11 bytes) with margin */
#define SYSFS_CONTENT_MAX 32

typedef struct sysfs_node {
    const char       *name;          /* caller-owned string pointer (not copied) */
    uint8_t           type;          /* SYSFS_NODE_DIR or SYSFS_NODE_FILE */
    struct sysfs_node *first_child;  /* first child (dirs only) */
    struct sysfs_node *next_sibling; /* next sibling in parent's child list */
    /* File content — callback takes priority over static buffer */
    int (*callback)(char *buf, uint32_t bufsiz);
    char              content[SYSFS_CONTENT_MAX];
    uint32_t          content_len;
} sysfs_node_t;

/* Global sysfs root node — drivers create children under this */
extern sysfs_node_t *g_sysfs_root;

void sysfs_init(void);
sysfs_node_t *sysfs_create_dir(sysfs_node_t *parent, const char *name);
sysfs_node_t *sysfs_create_file(sysfs_node_t *parent, const char *name,
                                 const char *buf, uint32_t len,
                                 int (*callback)(char *, uint32_t));

#endif /* _MINIOS_FS_SYSFS_H_ */
