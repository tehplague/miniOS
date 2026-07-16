# Storage in miniOS

The storage stack in miniOS is designed with a layered architecture, comprising a low-level disk driver, a cached block layer, a GPT partition table parser, a block device registry, a filesystem parser, and a virtual file system (VFS) to abstract file operations.

```
VFS (open / read / write / mount)
        │
   ext2 / tmpfs / sysfs / fat32
        │
  Block Device Registry   ← /dev/sda, /dev/sda1, ...
        │
   GPT Parser             ← LBA 1 header, LBA 2-33 entries
        │
   Block Layer (cache)
        │
   ATA DMA Driver
```

## ATA Driver

At the bottom of the stack is the ATA (Advanced Technology Attachment) driver. This driver is responsible for direct communication with the hard disk hardware.

- **Functionality**: It provides a basic function, `ata_read_sectors`, which reads a specified number of 512-byte sectors from the disk starting at a given Linear Block Address (LBA).
- **DMA**: The driver is configured to use Direct Memory Access (DMA) for efficient data transfer between the disk and memory.

## Block Layer

Between the low-level ATA driver and the higher-level filesystem drivers sits a cached block layer. This layer abstracts the disk driver and improves performance by caching frequently accessed blocks in memory.

- **Purpose**: The block layer provides a generic interface (`block_read`, `block_write`) for reading and writing 512-byte blocks, hiding the specifics of the underlying ATA driver.
- **Caching**: It maintains a 32 KiB in-memory cache for disk blocks (`CACHE_SIZE_BLOCKS = 64` × 512-byte sectors). This significantly speeds up read operations when the requested data is already in the cache.
- **Write Policy**: The cache operates in a **write-through** mode. When data is written, it is first written to the underlying disk and then the cache is updated. This ensures data integrity at the cost of slower write performance compared to a write-back cache.
- **Eviction Policy**: To make space for new blocks, the cache uses the **CLOCK algorithm**. This algorithm provides a good approximation of a Least Recently Used (LRU) policy with lower overhead. Each cache entry has a "referenced" bit. When a block needs to be evicted, the algorithm sweeps through the cache, looking for a block that has not been recently referenced.

## GPT Partition Table

miniOS parses a GUID Partition Table (GPT) at boot time to discover partitions on the first ATA disk. The GPT is the modern replacement for MBR partitioning and supports up to 128 partition entries with 64-bit LBA addressing.

- **Location**: The primary GPT header lives at LBA 1; partition entries occupy LBA 2–33 (128 entries × 128 bytes = 32 sectors). A backup copy is stored at the end of the disk.
- **Parsing** (`gpt_parse`): Reads LBA 1, validates the `"EFI PART"` signature and revision `0x00010000`, then iterates the entry table skipping empty slots (type GUID all-zeros). Returns an array of `gpt_partition_t` structs each holding `lba_start`, `lba_end`, and the partition type and unique GUIDs.
- **Disk image**: `scripts/create-gpt-disk.sh` writes the GPT onto `dist/x86_64/disk.img` using `sfdisk`. The ext2 payload is written at LBA 2048 (1 MiB alignment) before the partition table is applied, so `sfdisk` only touches the table sectors (LBA 0–33 and the backup at the end).

## Block Device Registry

After GPT parsing, each discovered partition — and the whole disk — is registered in the block device registry (`block_device.c`) and exposed as a device node in `/dev`.

| Device | major | minor | Description |
|--------|-------|-------|-------------|
| `/dev/sda`  | 8 | 0 | Whole first ATA disk |
| `/dev/sda1` | 8 | 1 | First GPT partition |
| `/dev/sda2` | 8 | 2 | Second GPT partition (if present) |
| …           | … | … | … |

Major/minor numbering follows the Linux convention for SCSI/ATA disks (major 8). The registry is queryable via `blkdev_find(major, minor)` and `blkdev_get(index)`. `SYS_mount` resolves the source path via `vfs_lstat`, extracts `(major, minor)` from the block device node's `dev` field, and passes them to `blkdev_find` to obtain the partition's LBA bounds.

## ext2 Filesystem

miniOS includes a read-write driver for the Second Extended Filesystem (ext2). This allows the kernel to read and write files and directories from a pre-prepared disk image.

- **Mounting**: The filesystem is mounted via `ext2_mount`, which reads the ext2 superblock, verifies its magic number, and initializes global filesystem parameters like block size and inode counts. The caller passes a `vfs_mount_data_t` with `major`/`minor`; the driver resolves LBA bounds via `blkdev_find()` internally.
- **File Operations**: The driver supports creating, reading, writing, and deleting files, as well as creating directories (`mkdir`), symbolic links (`symlink`), and device nodes (`mknod`). It also supports changing file permissions (`chmod`).
- **Path Resolution**: `ext2_lookup` traverses a directory tree to find the inode corresponding to a given path. It tokenizes the path by '/' and iteratively searches each directory.
- **Design**: The driver is designed to be simple and avoid dynamic memory allocation. It uses a single, static 4096-byte buffer for all block-reading operations.

## FAT32 Filesystem

miniOS includes a read-only FAT32 driver (`src/kernel/fs/fat32.c`) for the second GPT partition (`/dev/sda2`), mounted at `/data` by default.

- **Mounting**: `fat32_init()` registers the `"fat32"` type with the VFS registry. Mounting requires a `vfs_mount_data_t` with `major`/`minor`; LBA bounds are resolved internally via `blkdev_find()`. At mount time the driver reads the BPB from partition sector 0 and derives `g_first_data_sector`, `g_fat_start_sector`, `g_sectors_per_cluster`, and `g_root_cluster`.
- **Inode scheme**: The VFS inode number is the FAT32 starting cluster number of the file or directory. The root directory uses `bpb.ebr.root_directory_cluster` (typically 2).
- **Cluster chain**: `fat_entry(cluster)` reads one 512-byte FAT sector and returns the masked 28-bit next-cluster value. Chains terminate at `>= 0x0FFFFFF8`.
- **Directory iteration**: `fat32_scan_dir` walks the cluster chain sector-by-sector, accumulates up to 20 LFN slots into a UCS-2 buffer, validates the checksum against the following short entry, and calls the provided callback with the resolved name. Falls back to 8.3 names on checksum mismatch.
- **Long filenames**: LFN entries (attribute `0x0F`) are decoded from UCS-2 to ASCII; non-ASCII characters are replaced with `?`. Names up to 260 characters are supported.
- **File reads**: `fat32_read` skips to the target cluster via the FAT chain, then streams data one sector at a time into the caller's buffer.
- **Limitations**: Read-only — no write, create, unlink, mkdir, or symlink. Only 512-byte sectors are supported. Not reentrant (single static sector buffer shared between FAT reads and directory/data reads).

## tmpfs Filesystem

miniOS uses an in-memory temporary file system (`tmpfs`) for volatile storage, primarily for `/dev` and `/tmp`.

- **In-Memory**: All files and directories exist only in RAM and are lost on reboot.
- **Functionality**: `tmpfs` supports most standard file operations, including creating files, directories, symlinks, and device nodes.
- **Device Nodes**: The kernel creates essential device nodes at boot time: `/dev/null`, `/dev/tty`, `/dev/sda`, and `/dev/sda1` (plus any additional GPT partitions).

## Virtual File System (VFS)

The VFS provides a unified interface for file operations, decoupling the upper layers of the kernel from the underlying filesystem implementation.

- **Abstraction**: It defines a common file model and a set of standard file operations (e.g., read, write, open, close).
- **Integration**: The ext2 and tmpfs drivers integrate with the VFS, translating their specific data structures into generic VFS file types.
- **File Descriptors**: The VFS manages a system-wide table of open files and assigns file descriptors to user processes, including reserving standard I/O handles (stdin, stdout, stderr).
- **Device Numbers**: `vfs_inode_info_t` and `vfs_file_t` carry a `uint32_t dev` field encoding `(major << 8) | minor`. Filesystem drivers set it for `CHR`/`BLK` inodes; `vfs_open` copies it into the open file entry; `vfs_fstat` exposes it so `populate_stat` can write `st_rdev` and set the correct `S_IFBLK`/`S_IFCHR` mode bits.

## Filesystem Features

miniOS supports a variety of modern filesystem features, exposed through the VFS and syscalls.

### Syscall Interface

| Syscall | Number | Description |
|---------|--------|-------------|
| `SYS_symlink` | 88 | Create a symlink pointing to a target path |
| `SYS_readlink`| 89 | Read a symlink target into a user buffer |
| `SYS_lstat`   | 6  | Stat a file without following the final symlink component |
| `SYS_mknod`   | 133| Create a special or ordinary file |
| `SYS_chmod`   | 90 | Change permissions of a file |

`SYS_lstat` reuses `vfs_find_mount + ops->lookup` directly without calling
`vfs_resolve_path`, so it reports the symlink inode itself rather than the target.
