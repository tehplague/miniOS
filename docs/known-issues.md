# Known Issues and Hobby OS Characteristics

miniOS is an educational x86_64 hobby kernel. This page documents the known limitations and deliberate simplifications that distinguish it from a general-purpose production OS such as Linux, FreeBSD, or macOS. Each section notes what is missing, why it matters, and what a real OS does instead.

---

## Memory Management

### No swap / virtual memory beyond physical RAM
miniOS has no swap device. When physical memory is exhausted `pmm_alloc_frame()` returns 0 and allocations fail. A production OS pages cold data out to disk transparently. Anonymous `mmap` and `brk` growth are demand-paged (pages allocated on first fault); file-backed `mmap` reads page content from the filesystem on first access.

### 64 MiB kernel heap ceiling
The boundary-tag allocator (`kmalloc`/`kfree`) is capped at `HEAP_MAX - HEAP_START = 64 MiB`. The heap grows on demand page-by-page up to that ceiling. Fragmentation over long uptimes can cause allocations to fail even if total free bytes would satisfy the request.

### Bitmap PMM — no buddy allocator
The physical memory manager uses a flat bitmap. There is no buddy system; `pmm_alloc_frame()` is O(n) scan per allocation. `pmm_alloc_contiguous(n)` scans for `n` consecutive free frames (also O(n)) and is used for DMA virtqueue allocation. For large or frequent multi-page allocations a buddy allocator would give O(log n) behaviour and reduced fragmentation.

### Partial `mprotect`; no `mremap` or `msync`
`mprotect(2)` is implemented (`SYS_mprotect = 10`): it remaps every page in the given range with PTE flags derived from the `prot` argument. Because miniOS does not set the NX bit in PTEs, `PROT_NONE` and `PROT_READ` are both enforced as read-only (no write); execute permission cannot be revoked. `mremap` and `msync` return `ENOSYS`. `mmap` supports only `MAP_PRIVATE|MAP_ANONYMOUS` and private read-only file mappings at `offset == 0`. `MAP_SHARED` (shared memory between processes), non-zero offsets all return `EINVAL`.

### Fixed mmap region table (64 entries)
Each process can hold at most `MMAP_MAX_REGIONS = 64` mapped regions simultaneously. Programs that call `mmap`/`munmap` in tight loops (e.g., allocators using `mmap` as a backing store) will exhaust the table and receive `ENOMEM`.

### Fixed user stack (64 KiB, no guard page)
The user stack is allocated as `USER_STACK_PAGES = 16` × 4 KiB pages at a fixed address below `USER_STACK_TOP`. Stack overflow silently corrupts adjacent memory — there is no guard page and no automatic stack growth.

### Partial ASLR — no heap or text randomisation
miniOS randomises two regions per exec: (1) the mmap base is offset 0–255 pages below
`MMAP_BASE` using RDTSC entropy; (2) the initial RSP is displaced 0–4080 bytes (16-byte
aligned) below `USER_STACK_TOP`. These make library and stack addresses unpredictable
between runs. The heap base (`brk_base`) is fixed at the page-aligned end of the ELF
`PT_LOAD` segments and cannot be randomised without PIE executables. Text (code) loads
at a fixed address (0x400000) for the same reason. A production OS randomises all four
regions using position-independent code.

---

## Scheduler

### Round-robin only — no priorities or nice values
The per-CPU run queues are plain FIFOs. There is no CFS, no O(1) scheduler, no priority levels, and no `nice`/`setpriority`. All runnable threads share equal CPU time within a CPU's quantum (10 ms LAPIC timer tick).

### Shared-memory threads via `clone`; no kernel threads beyond idle
`clone(CLONE_VM|CLONE_FILES|CLONE_THREAD|CLONE_SETTLS, ...)` is implemented and creates user threads sharing fd_table, heap, mmap regions, and cwd via a `thread_group_t`. The `futex` syscall (FUTEX_WAIT/FUTEX_WAKE) is implemented, enabling mlibc pthreads. Kernel-mode threads beyond the per-CPU idle threads are not supported. The thread pool is statically sized (`SCHED_MAX_THREADS = MAX_CPUS + 8`), capping concurrent threads at 16 with `MAX_CPUS = 8`.

### Hard limit of 16 concurrent threads
The thread pool is statically sized at `SCHED_MAX_THREADS = MAX_CPUS + 8 = 16` entries. Creating more than 8 user threads (accounting for per-CPU idle threads) fails.

### No real-time scheduling classes
`SCHED_FIFO` and `SCHED_RR` are not implemented. There is no way to give a thread a hard latency guarantee.

---

## Filesystem

### ext2 without journaling
The ext2 implementation does not journal metadata. An unclean shutdown (power loss, triple fault) can leave the on-disk filesystem in an inconsistent state with no automatic recovery path. A production filesystem (ext4, XFS, ZFS) would replay its journal on next mount.

### No `fsck` / filesystem check tool
There is no `fsck` equivalent. Corruption detected at runtime produces a kernel message and an I/O error — the filesystem is not repaired automatically.

### Tiny block cache (32 KiB, read-only)
The block layer cache holds `CACHE_SIZE_BLOCKS = 64` × 512-byte sectors = 32 KiB, evicted via a simple clock algorithm. Writes bypass the cache entirely (write-through to the ATA layer). A production OS uses a much larger page cache unified with the VM subsystem and employs write-back with controlled writeback throttling.

### 64 open files per process
`VFS_MAX_FDS = 64`. Programs that open many files simultaneously (e.g., compilers, interpreters) may still hit this limit. Linux defaults to 1024 and allows raising to millions.

### Missing file syscalls
The following POSIX file operations are not implemented: `fallocate`, `sendfile`, `splice`, `copy_file_range`. `O_EXCL` on `open` is ignored (no atomic create-exclusive). `ftruncate` is fully implemented on tmpfs (shrink frees blocks and zeroes the tail; extend is sparse — size updated, blocks allocated on first access). The ext2 truncate stub returns 0 for truncate-to-zero and `ENOSYS` for non-zero lengths (ext2 is read-only). `rename(2)` is implemented for ext2 and tmpfs; cross-filesystem renames return `EXDEV`, and overwriting a non-empty directory destination returns `ENOTEMPTY`.

### No `epoll` / `kqueue` — only `poll` and `select`
Event notification for large numbers of file descriptors uses `poll` and `select`, which scan all descriptors on every call (O(n)). There is no `epoll` / `kqueue` (O(1) readiness notification) needed by production servers.

### No inotify / file-system event notification
There is no mechanism for a process to be notified of changes to files or directories made by other processes.

---

## Process Model and IPC

### Threads via `clone`; no true multi-process shared memory
User threads are created via `clone(CLONE_VM|CLONE_FILES|CLONE_THREAD, ...)` which shares
the fd table, heap, mmap regions, and cwd through a `thread_group_t`. `pthread_create` and
`pthread_join` work via mlibc. Inter-*process* shared writable memory (between separate
`fork()` children) is not supported: `MAP_SHARED` returns `EINVAL` and forked processes
get private physical frame copies of the code/data region.

### No POSIX shared memory
`shm_open`, `shmget`, `shmat` are not implemented. Inter-process data sharing requires pipes or files.

### No System V IPC
Message queues (`msgget`/`msgsnd`/`msgrcv`), semaphore sets (`semget`/`semop`), and shared memory (`shmget`/`shmat`) are absent. Pipes are the only IPC primitive beyond sockets.

### `futex` supports only FUTEX_WAIT and FUTEX_WAKE
`futex(2)` is implemented with `FUTEX_WAIT` and `FUTEX_WAKE` (including the `FUTEX_PRIVATE` flag variant). Advanced operations (`FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`, `FUTEX_WAIT_BITSET`, PI futexes) return `ENOSYS`. Robust futexes (owner-death notification) are not supported.

### No process groups for job control beyond basics
`setpgid`/`setsid` are implemented. `SIGSTOP`/`SIGTSTP`/`SIGCONT` work (a stopped thread
is a distinct scheduler state, `THREAD_STOPPED`, and `wait4`/`waitpid` support
`WUNTRACED`/`WNOHANG` with real `WIFSTOPPED`/`WSTOPSIG` status encoding — see
`docs/scheduler.md`), but the shell's terminal-driven job-control features
(`SIGTTOU`/`SIGTTIN`, `fg`/`bg`, foreground process group tracking) are not — BusyBox
`ash` in this build has `CONFIG_ASH_JOB_CONTROL=n` and gets stuck on its own plain
blocking `wait()` if a job it started is stopped, since it isn't `WUNTRACED`-aware.

---

## Security

### Partial ASLR (see also Memory Management)
mmap base and stack RSP are randomised per exec using RDTSC entropy. Text (0x400000) and
heap base are fixed; full ASLR requires PIE executables. See the Memory Management section
for details.

### Partial SMEP / SMAP enforcement
SMEP (CR4 bit 20) is enabled at boot when the CPU reports support (CPUID leaf 7, EBX bit 7), preventing the kernel from executing user-space pages. SMAP (CR4 bit 21) is enabled when supported (EBX bit 20); the AC flag is managed via `pushfq`/`popfq` around syscall dispatch so kernel code can access user buffers only inside the dispatcher. QEMU `-cpu Haswell` does not expose SMAP, so it remains inactive on the default test configuration. A production kernel additionally uses `copy_from_user`/`copy_to_user` wrappers to enforce SMAP at every individual user-buffer access rather than opening a wide window across the full dispatcher.

### No user / group model
There are no UIDs or GIDs. All processes have equivalent privilege (treated as root). File
permission bits are stored in ext2 and tmpfs inodes; `chmod` is implemented. `vfs_open`
enforces permission bits in three places:
- **Write access**: returns `EACCES` if the target file has no write bits and `O_WRONLY`/`O_RDWR` is requested.
- **Directory write**: returns `EACCES` on `O_CREAT` if the parent directory has no write bits.
- **Execute**: `sys_execve` returns `EACCES` if the file has no execute bits.
Read access is always granted (consistent with Linux root behaviour). The `mode` argument to
`open(2)` with `O_CREAT` is honoured: ext2 and tmpfs store the requested permission bits
(lower 9 bits) in the new inode rather than a hardcoded 0644. sysfs and fat32 files are
marked read-only (0444/0444, dirs 0555) at the VFS layer.

### No capabilities or mandatory access control
Linux capabilities, SELinux, AppArmor, and similar MAC frameworks have no equivalent here. Any process can call any syscall.

### Static, position-dependent executables only
All userspace binaries are statically linked against mlibc at fixed virtual addresses. There is no dynamic linker, no shared libraries, and no position-independent code (PIE). This makes binaries larger and prevents address-layout hardening.

---

## Networking

### IPv4 only
The lwIP port is configured for IPv4 only. There is no IPv6 stack, no dual-stack support, and no `AF_INET6` socket family.

### Two NIC types; no hot-plug or multi-NIC
`net_init()` probes for virtio-net (vendor `0x1AF4`/device `0x1000`) first, then falls back to RTL8139 (`0x10EC`/`0x8139`). Only one NIC is registered; a second device of either type is silently ignored. No hot-plug, no `e1000` or other drivers. Running on real hardware requires either an RTL8139 card or a virtio-compatible hypervisor.

### AF_UNIX SOCK_STREAM only; no SOCK_DGRAM, no SCM_RIGHTS
`AF_UNIX` `SOCK_STREAM` sockets are implemented (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `socketpair`). `SOCK_DGRAM` returns `EPROTONOSUPPORT`. `sendmsg`/`recvmsg` with `SCM_RIGHTS` (file-descriptor passing between processes) is not supported — only `send`/`recv` byte-stream transfer. Each socket has a 4 KiB kernel-side ring buffer; large messages must be fragmented by the application.

### No non-blocking I/O on sockets beyond `poll`/`select`
`O_NONBLOCK` on sockets is not fully implemented. The `fcntl(F_SETFL, O_NONBLOCK)` path exists but socket send/recv paths may still block inside lwIP.

### No AAAA records in DNS resolver
Name resolution uses a userspace RFC 1035 resolver (`resolve_ipv4_hostname` in `user/libc/syscalls.c`) that reads `/etc/resolv.conf` and sends a UDP A-record query directly. CNAME chains are followed within the response (up to 8 hops). IPv6 (AAAA), MX, and SRV record types are not supported.

---

## Hardware Support

### x86_64 only, up to 8 CPUs
The entire architecture layer targets 64-bit x86. There is no ARM, RISC-V, or 32-bit x86 port. SMP is capped at `MAX_CPUS = 8` (rebuild with `-DMAX_CPUS=N` to raise, up to the ticket-spinlock's 256-waiter limit).

### Single fixed-size console; no multi-head or resolution switching
The console is either a 160×50 GOP linear framebuffer (32bpp, 1280×800, LatArCyrHeb-16 8×16 font) when GRUB provides a Multiboot2 framebuffer tag, or an 80×25 VGA text-mode fallback. There is no runtime resolution change, no multi-monitor support, no GPU acceleration, and no DRM/KMS layer. A production OS would expose a DRM device node and allow userspace to set display modes.

### No USB
There is no USB host controller driver (UHCI/EHCI/xHCI). USB keyboards and mass storage devices are not supported.

### No sound
No audio subsystem (HDA, AC'97, etc.) exists.

### No ACPI power management beyond MADT
ACPI is used only to parse the Multiple APIC Description Table (MADT) during SMP boot. There is no ACPI power management, no sleep states (S3/S4/S5), no CPU frequency scaling, and no battery monitoring.

### ATA DMA limited to 8 sectors per transfer
The ATA driver uses PCI Bus Mastering DMA (`READ/WRITE DMA EXT`) with a single 4 KiB bounce buffer and a one-entry PRD table. This caps transfers at 8 sectors (4 KiB) per call and requires a bounce-copy for every I/O. A production driver would use a multi-entry scatter-gather PRD table and transfer directly from/to caller buffers without bouncing.
