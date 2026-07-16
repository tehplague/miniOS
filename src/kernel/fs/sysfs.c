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

/* sysfs.c — Kernel-managed read-only virtual filesystem.
 *
 * Storage model:
 *   - Nodes: heap-allocated sysfs_node_t structs forming an intrusive linked list tree.
 *   - The tree root is g_sysfs_root (a directory node with name "").
 *   - Children are stored as a prepend-ordered singly-linked list via first_child/next_sibling.
 *   - Inodes are small integers (1-based) into g_sysfs_nodes[]. Raw pointer-as-ino
 *     is unsafe: kernel addresses are 64-bit and the VFS ino field is only uint32_t.
 *
 * Path resolution:
 *   sysfs_lookup() receives a RELATIVE path (mount point already stripped by VFS router).
 *   An empty string ("") means the sysfs root itself.
 *   Path components are split on '/'; each component scans the current node's child list.
 *
 * File content:
 *   Files may provide content via a static buffer (content[]/content_len)
 *   or a dynamic callback() callback. The callback takes priority when non-NULL.
 */

#include <miniOS/fs/sysfs.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/mm/heap.h>
#include <miniOS/io.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------------- */

sysfs_node_t *g_sysfs_root = NULL;

static vfs_ops_t sysfs_ops;

/* Inode table — maps 1-based uint32_t inode IDs to node pointers.
 * Avoids truncating 64-bit kernel pointers to the 32-bit VFS ino field. */
#define SYSFS_MAX_NODES 256
static sysfs_node_t *g_sysfs_nodes[SYSFS_MAX_NODES]; /* index 0 unused */
static uint32_t      g_sysfs_next_ino = 1;

static uint32_t sysfs_node_to_ino(sysfs_node_t *node) {
    /* Return existing ID if already registered */
    for (uint32_t i = 1; i < g_sysfs_next_ino; i++) {
        if (g_sysfs_nodes[i] == node) return i;
    }
    if (g_sysfs_next_ino >= SYSFS_MAX_NODES) return 0; /* table full */
    uint32_t id = g_sysfs_next_ino++;
    g_sysfs_nodes[id] = node;
    return id;
}

static sysfs_node_t *sysfs_ino_to_node(uint32_t ino) {
    if (ino == 0 || ino >= g_sysfs_next_ino) return NULL;
    return g_sysfs_nodes[ino];
}

/* -----------------------------------------------------------------------
 * Node creation helpers
 * --------------------------------------------------------------------- */

sysfs_node_t *sysfs_create_dir(sysfs_node_t *parent, const char *name) {
    sysfs_node_t *node = (sysfs_node_t *)kmalloc(sizeof(sysfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->name = name;  /* caller-owned, NOT copied */
    node->type = SYSFS_NODE_DIR;
    sysfs_node_to_ino(node); /* register in inode table */

    if (parent) {
        /* Prepend to parent's child list */
        node->next_sibling = parent->first_child;
        parent->first_child = node;
    }
    return node;
}

sysfs_node_t *sysfs_create_file(sysfs_node_t *parent, const char *name,
                                 const char *buf, uint32_t len,
                                 int (*callback)(char *, uint32_t)) {
    sysfs_node_t *node = (sysfs_node_t *)kmalloc(sizeof(sysfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->name = name;  /* caller-owned, NOT copied */
    node->type = SYSFS_NODE_FILE;
    node->callback = callback;
    sysfs_node_to_ino(node); /* register in inode table */

    if (callback == NULL)
    {
        if (len > SYSFS_CONTENT_MAX) len = SYSFS_CONTENT_MAX;
        if (buf && len > 0) {
            memcpy(node->content, buf, len);
        }
        node->content_len = len;
    }

    /* Prepend to parent's child list */
    node->next_sibling = parent->first_child;
    parent->first_child = node;

    return node;
}

sysfs_node_t *sysfs_create_file_show(sysfs_node_t *parent, const char *name,
                                      int (*callback)(char *buf, uint32_t bufsiz)) {
    sysfs_node_t *node = (sysfs_node_t *)kmalloc(sizeof(sysfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->name = name;  /* caller-owned, NOT copied */
    node->type = SYSFS_NODE_FILE;
    node->callback = callback;
    sysfs_node_to_ino(node); /* register in inode table */

    /* Prepend to parent's child list */
    node->next_sibling = parent->first_child;
    parent->first_child = node;

    return node;
}

/* -----------------------------------------------------------------------
 * VFS ops
 * --------------------------------------------------------------------- */

static int sysfs_lookup(const char *path, vfs_inode_info_t *out) {
    sysfs_node_t *node = g_sysfs_root;
    if (!node) return -1;

    /* Empty path — return root info */
    if (!path || path[0] == '\0') {
        out->inode     = sysfs_node_to_ino(g_sysfs_root);
        out->file_type = VFS_FILE_TYPE_DIR;
        out->size      = 0;
        out->mode      = 0555;
        return 0;
    }

    /* Walk path components split on '/' */
    const char *p = path;
    while (*p) {
        /* Skip leading slash(es) */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* Find end of this component */
        const char *end = p;
        while (*end && *end != '/') end++;
        uint32_t comp_len = (uint32_t)(end - p);

        /* Skip "." components (stay at current node) */
        if (comp_len == 1 && p[0] == '.') { p = end; continue; }

        /* Search current node's children for this component */
        sysfs_node_t *child = node->first_child;
        sysfs_node_t *found = NULL;
        while (child) {
            if (child->name &&
                strncmp(child->name, p, comp_len) == 0 &&
                child->name[comp_len] == '\0') {
                found = child;
                break;
            }
            child = child->next_sibling;
        }

        if (!found) return -1;
        node = found;
        p = end;
    }

    out->inode     = sysfs_node_to_ino(node);
    out->file_type = (node->type == SYSFS_NODE_DIR) ? VFS_FILE_TYPE_DIR : VFS_FILE_TYPE_REG;
    out->size      = node->content_len;
    out->mode      = (node->type == SYSFS_NODE_DIR) ? 0555 : 0444;
    return 0;
}

static int sysfs_read(uint32_t ino, uint64_t off, void *buf, uint32_t len) {
    sysfs_node_t *node = sysfs_ino_to_node(ino);
    if (!node || node->type != SYSFS_NODE_FILE) return -1;

    /* Dynamic callback takes priority */
    if (node->callback) {
        return node->callback((char *)buf, len);
    }

    /* Static content buffer */
    if (off >= node->content_len) return 0;
    uint32_t avail = node->content_len - (uint32_t)off;
    if (len > avail) len = avail;
    memcpy(buf, node->content + off, len);
    return (int)len;
}

static int sysfs_readdir(uint32_t ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud) {
    sysfs_node_t *node = sysfs_ino_to_node(ino);
    if (!node || node->type != SYSFS_NODE_DIR) return -1;

    uint64_t pos = 0;
    sysfs_node_t *child = node->first_child;
    while (child) {
        if (pos >= *offset) {
            uint8_t ftype = (child->type == SYSFS_NODE_DIR) ? VFS_FILE_TYPE_DIR : VFS_FILE_TYPE_REG;
            int r = cb(child->name, (uint8_t)strlen(child->name),
                       sysfs_node_to_ino(child), ftype, ud);
            (*offset)++;
            if (r != 0) return r;
        }
        pos++;
        child = child->next_sibling;
    }
    return 0;
}

static int sysfs_unlink(uint32_t parent_ino, const char *name) {
    (void)parent_ino;
    (void)name;
    return -30; /* EROFS */
}

static int sysfs_rmdir(uint32_t parent_ino, const char *name) {
    (void)parent_ino;
    (void)name;
    return -30; /* EROFS */
}

/* -----------------------------------------------------------------------
 * Mount callback — called by vfs_mount_fstype() when "sysfs" is requested
 * --------------------------------------------------------------------- */

static int sysfs_statfs(vfs_statfs_t *out)
{
    memset(out, 0, sizeof(*out));
    out->f_type    = 0x62656572; /* SYSFS_MAGIC */
    out->f_bsize   = 4096;
    out->f_namelen = 255;
    return 0;
}

static int sysfs_mount(const char *source, const char *target, const void *data) {
    (void)source; (void)data;
    return vfs_register_mount(target, &sysfs_ops, sysfs_node_to_ino(g_sysfs_root));
}

/* -----------------------------------------------------------------------
 * sysfs_init — create root node and register "sysfs" filesystem type
 * --------------------------------------------------------------------- */

void sysfs_init(void) {
    /* Create root directory node (no parent) */
    g_sysfs_root = sysfs_create_dir(NULL, "");

    /* Fill ops table — read-only fs, no write/create/unlink/mkdir needed */
    memset(&sysfs_ops, 0, sizeof(sysfs_ops));
    sysfs_ops.lookup  = sysfs_lookup;
    sysfs_ops.read    = sysfs_read;
    sysfs_ops.readdir = sysfs_readdir;
    sysfs_ops.unlink  = sysfs_unlink;
    sysfs_ops.rmdir   = sysfs_rmdir;
    sysfs_ops.statfs  = sysfs_statfs;

    register_filesystem("sysfs", sysfs_mount);
    printk("sysfs: filesystem type registered\n");
}
