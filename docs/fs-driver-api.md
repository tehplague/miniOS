# Filesystem Driver API

miniOS uses a two-layer registration model: a **filesystem type registry** maps names like
`"tmpfs"` to mount callbacks, and a **mount table** maps path prefixes like `"/tmp"` to
`vfs_ops_t` operation tables. Adding a new filesystem means implementing `vfs_ops_t`,
writing a mount callback, and registering with `register_filesystem()`.

---

## Concepts

### Filesystem type registry

`register_filesystem(name, mount_cb)` — called once at kernel init — records a name→callback
mapping. When `mount("tmpfs", ...)` arrives from userspace (or from `main.c`), the VFS calls
`vfs_mount_fstype("tmpfs", source, target, data)`, which looks up the name and invokes the
callback. Up to `VFS_MAX_FS_TYPES = 16` types can be registered.

### Mount table

`vfs_register_mount(point, ops, root_ino)` — called inside the mount callback — writes a
`vfs_mount_t` entry that binds a path prefix to a `vfs_ops_t *`. The VFS router uses
longest-prefix matching: opening `/tmp/foo` finds the `/tmp` entry and passes `"foo"` (relative)
to that filesystem's `lookup()`. Up to `VFS_MAX_MOUNTS = 16` mounts are supported.

### `vfs_ops_t` — per-mount operation table

```c
typedef struct vfs_ops {
    int  (*lookup)  (const char *path, vfs_inode_info_t *out);
    int  (*read)    (uint32_t ino, uint64_t off, void *buf, uint32_t len);
    int  (*readdir) (uint32_t ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud);
    int  (*write)   (uint32_t ino, uint64_t off, const void *buf, uint32_t len);
    int  (*flush)   (uint32_t ino);
    int  (*create)  (uint32_t parent_ino, const char *name, uint32_t *new_ino_out);
    int  (*unlink)  (uint32_t parent_ino, const char *name);
    int  (*rename)  (uint32_t old_parent_ino, const char *old_name,
                     uint32_t new_parent_ino, const char *new_name);
    int  (*mkdir)   (uint32_t parent_ino, const char *name, uint32_t *new_ino_out);
    int  (*rmdir)   (uint32_t parent_ino, const char *name);
    void (*set_root)(uint32_t root_ino);
    int  (*truncate)(uint32_t ino, uint64_t new_size);
    int  (*readlink)(uint32_t ino, char *buf, uint32_t len);
    int  (*symlink) (uint32_t parent_ino, const char *name, const char *target);
    int  (*mknod)   (uint32_t parent_ino, const char *name, uint16_t mode,
                     uint32_t dev, uint32_t *new_ino_out);
    int  (*chmod)   (uint32_t ino, uint16_t mode);
} vfs_ops_t;
```

NULL pointers are safe for operations the filesystem does not support. The VFS checks for NULL
before calling and returns `-ENOSYS` / `-EROFS` to the caller.

The `readdir` callback type `vfs_dirent_cb_t` is defined in `<miniOS/fs/fs_types.h>` (included
transitively via `<miniOS/fs/vfs.h>`):

```c
typedef int (*vfs_dirent_cb_t)(const char *name, uint8_t name_len,
                                uint32_t inode, uint8_t file_type, void *ud);
```

**Operation contract:**

| Op | Receives | Returns |
|----|----------|---------|
| `lookup` | path relative to mount point (`""` = root) | 0 / −1 |
| `read` | inode number, byte offset, buffer, len | bytes read / −1 |
| `readdir` | dir inode, cursor `*offset`, callback | 0 or callback's non-zero |
| `write` | inode number, byte offset, buffer, len | bytes written / −1 |
| `flush` | inode number | 0 / −1 |
| `create` | parent inode, filename (no `/`) | 0 / −errno |
| `unlink` | parent inode, filename | 0 / −errno |
| `rename` | old parent inode + name, new parent inode + name | 0 / −errno |
| `mkdir` | parent inode, name | 0 / −errno |
| `rmdir` | parent inode, name | 0 / −errno |
| `set_root` | root inode for this mount | void |
| `truncate` | inode number, new size in bytes (0 = truncate-to-zero) | 0 / −errno |
| `readlink` | symlink inode, buf, len | bytes / −1 |
| `symlink` | parent inode, name, target string | 0 / −1 |
| `mknod` | parent inode, name, mode, dev | 0 / −errno |
| `chmod` | inode, mode bits | 0 / −1 |

`set_root` is required for filesystems that support multiple independent mounts from the same
inode pool (e.g. tmpfs). It is called by the VFS router before every `lookup()` to set the
per-mount root inode. Single-root filesystems (ext2, sysfs) leave it NULL.

---

## Adding a new filesystem — step by step

### 1. Create the driver files

```
include/miniOS/fs/myfs.h   ← public API
src/kernel/fs/myfs.c       ← implementation
```

Include `<miniOS/fs/vfs.h>` in both. The header needs at minimum:

```c
/* myfs.h */
#ifndef _MINIOS_FS_MYFS_H_
#define _MINIOS_FS_MYFS_H_
#include <miniOS/fs/vfs.h>

/* Called once at kernel startup; registers the "myfs" filesystem type. */
void myfs_init(void);
#endif
```

### 2. Implement `vfs_ops_t`

In `myfs.c`, define a static ops table and implement only the operations your filesystem
supports. Read-only example:

```c
#include <miniOS/fs/myfs.h>
#include <miniOS/fs/vfs.h>

static int myfs_lookup(const char *path, vfs_inode_info_t *out) {
    /* resolve path relative to mount root; populate out->inode, file_type, size */
    ...
}

static int myfs_read(uint32_t ino, uint64_t off, void *buf, uint32_t len) {
    ...
}

static int myfs_readdir(uint32_t ino, uint64_t *offset,
                        vfs_dirent_cb_t cb, void *ud) {
    ...
}

static vfs_ops_t g_myfs_ops = {
    .lookup  = myfs_lookup,
    .read    = myfs_read,
    .readdir = myfs_readdir,
    /* all other fields left NULL — read-only filesystem */
};
```

### 3. Write the mount callback

The mount callback is invoked by `vfs_mount_fstype()`. It must call `vfs_register_mount()`
before returning. For a single-instance filesystem:

```c
static int myfs_mount(const char *source, const char *target, const void *data) {
    (void)source;
    /* cast data to your options struct if needed */
    if (myfs_internal_init() != 0)
        return -1;
    return vfs_register_mount(target, &g_myfs_ops, 0 /* use fs default root */);
}
```

For a filesystem that can be mounted multiple times (like tmpfs), allocate a fresh root
inode per mount and pass it as the third argument to `vfs_register_mount()`. The VFS will
call `set_root(root_ino)` before each `lookup()` on that mount:

```c
static int myfs_mount(const char *source, const char *target, const void *data) {
    (void)source; (void)data;
    myfs_init_pool();                      /* idempotent */
    uint32_t root = myfs_alloc_root();
    if (!root) return -12;                 /* ENOMEM */
    return vfs_register_mount(target, &g_myfs_ops, root);
}

static void myfs_set_root(uint32_t root_ino) {
    g_myfs_active_root = root_ino;
}
/* add .set_root = myfs_set_root in g_myfs_ops */
```

### 4. Register in `myfs_init()`

```c
void myfs_init(void) {
    register_filesystem("myfs", myfs_mount);
}
```

### 5. Wire into kernel startup (`src/kernel/main.c`)

Call `myfs_init()` before the first mount attempt. Add the include and the call:

```c
#include <miniOS/fs/myfs.h>

/* inside kernel_main(), before the first vfs_mount_fstype("myfs", ...) call: */
myfs_init();
```

If you want the kernel to auto-mount it at boot (before init runs), add:

```c
vfs_mount_fstype("myfs", NULL, "/mypoint", NULL);
```

The mount point directory must already exist in the root filesystem (create it in the ext2
image) or be a prefix that the VFS router can match.

### 6. Add to the Makefile

```makefile
KERNEL_OBJS += build/kernel/fs/myfs.o
```

---

## Adding an automatic mount in init

`user/init/main.c` runs as PID 1. It already mounts `/sys` via a raw `SYS_mount` syscall
(nr 165). Add further mounts the same way:

```c
/* raw mount syscall — no libc wrapper available */
static long sys_mount(const char *source, const char *target,
                      const char *fstype, unsigned long flags,
                      const void *data)
{
    long ret;
    register long r10 __asm__("r10") = flags;
    register long r8  __asm__("r8")  = (long)data;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"((long)165), "D"(source), "S"(target), "d"(fstype),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

int main(...) {
    sys_mount("none", "/sys",     "sysfs", 0, NULL);
    sys_mount("none", "/mypoint", "myfs",  0, NULL);
    ...
}
```

The filesystem type (`"myfs"`) must already be registered by the kernel via `myfs_init()`.
Userspace cannot register new filesystem types — only the kernel can call
`register_filesystem()`.

**Rule of thumb:** Mount in `main.c` (kernel) if the filesystem must exist before `init`
starts (e.g. `/dev`, `/tmp`). Mount in `init` if it requires the root filesystem to be up
first (e.g. `/sys` needs `/sys` directory in ext2).

---

## Existing filesystem drivers

| Type name | Source | Mount points | Notes |
|-----------|--------|--------------|-------|
| `ext2`  | `src/kernel/fs/ext2.c`  | `/`            | Persistent; requires `vfs_mount_data_t` |
| `tmpfs` | `src/kernel/fs/tmpfs.c` | `/tmp`, `/dev` | Multi-root; each mount gets an isolated inode pool |
| `sysfs` | `src/kernel/fs/sysfs.c` | `/sys`         | Read-only; tree built by drivers via `sysfs_create_*()` |
| `fat32` | `src/kernel/fs/fat32.c` | `/data` (or any) | Read-only; requires `vfs_mount_data_t`; LFN + 8.3 names; inode = starting cluster |

### Block-backed filesystems (ext2, fat32)

Both drivers accept a `vfs_mount_data_t` (defined in `include/miniOS/fs/fs_types.h`) that
identifies the block device by major/minor number.  The driver calls `blkdev_find()` internally
to resolve the LBA bounds — the caller never handles raw sector numbers.

```c
#include <miniOS/fs/fs_types.h>
#include <miniOS/drivers/block_device.h>

vfs_mount_data_t opts = { .major = BLKDEV_MAJOR_SATA, .minor = 1 };
vfs_mount_fstype("ext2", NULL, "/", &opts);
```

### tmpfs — no mount data needed

```c
vfs_mount_fstype("tmpfs", NULL, "/run", NULL);
```

Each call allocates a fresh, empty root directory. All tmpfs mounts share the same global
inode pool (`TMPFS_MAX_INODES = 256` across all mounts).

### sysfs — no mount data needed

```c
vfs_mount_fstype("sysfs", NULL, "/sys", NULL);
```

Populate the tree before or after mounting using `sysfs_create_dir()` /
`sysfs_create_file_show()` with `g_sysfs_root` as the root anchor.
