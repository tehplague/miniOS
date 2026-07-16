# miniOS Project Overview

miniOS is a hobby x86_64 kernel built from scratch as a learning vehicle for OS internals.
It boots via GRUB Multiboot 2, runs in 64-bit long mode mapped to the higher half of the
address space, and ships a complete symmetric multiprocessing (SMP) OS stack supporting up
to `MAX_CPUS=8` CPUs: LAPIC/IOAPIC interrupt routing, physical + virtual memory management
with demand paging and CoW fork, preemptive per-CPU round-robin scheduling with work-stealing,
ext2/tmpfs/sysfs/fat32 filesystems, POSIX process model (fork/exec/wait/signals/pipes),
user threads via `clone`+`futex` (mlibc pthreads), AF_UNIX SOCK_STREAM sockets, partial
ASLR (mmap base + stack RSP randomised per exec), file permission enforcement (mode bits
checked at open and exec), a VT/ANSI-aware terminal stack with GOP linear framebuffer
console, a UDP/TCP network stack (lwIP over virtio-net, POSIX socket syscalls), and a
userspace built against mlibc with Busybox and custom networking tools.

## Architecture

```
+--------------------------------------------------------------------------+
|                              Userspace (ring 3)                          |
|  +---------+ +----------+ +----------+ +--------+ +-------------------+ |
|  | mlibc   | | ELF Ldr  | | Busybox  | | Apps   | | nc wget ping init | |
|  +---------+ +----------+ +----------+ +--------+ +-------------------+ |
|----------------------  Syscall (SYSCALL/SYSRET, Linux ABI)  ------------|
|                              Kernel (ring 0)                             |
|  +--------+ +-------+ +-------+ +--------+ +--------+ +--------------+ |
|  | VFS    | | ext2  | | tmpfs | | sysfs  | | fat32  | | Block Cache  | |
|  +--------+ +-------+ +-------+ +--------+ +--------+ +--------------+ |
|  +--------+ +-------+ +-------+ +--------+ +--------+ +--------------+ |
|  | Per-CPU| | Work- | | PMM   | | VMM    | | Heap   | | TTY/VT/GOP   | |
|  | Sched  | | Steal | | Bitmap| | Paging | | 64 MiB | | Framebuffer  | |
|  +--------+ +-------+ +-------+ +--------+ +--------+ +--------------+ |
|  +--------+ +-------+ +-------+ +--------+ +--------+ +--------------+ |
|  | SMP    | | LAPIC | | IOAPIC| | Spin-  | | AF_UNIX| | Net sockets  | |
|  | ACPI   | | Timer | | IRQ   | | lock   | | stream | | lwIP/virtio  | |
|  +--------+ +-------+ +-------+ +--------+ +--------+ +--------------+ |
+--------------------------------------------------------------------------+
|                                  Hardware                                 |
|  x86_64 CPUs (≤8)  |  RAM  |  GPT disk (ATA DMA)  |  GOP/VGA display  |
|  PS/2 keyboard  |  virtio-net NIC  (RTL8139 fallback)                   |
+--------------------------------------------------------------------------+
```

## Subsystems

- **Bootloader**: GRUB Multiboot 2; jumps to 64-bit long mode via a 32-bit trampoline; passes
  memory map and optional framebuffer tag to the kernel.

- **Memory Management**: PMM (bitmap frame allocator, dynamic size — covers only detected RAM);
  `pmm_alloc_contiguous(n)` for DMA; VMM (4-level paging, higher-half kernel at
  `0xFFFF800000000000`); kernel heap (boundary-tag allocator, 64 MiB cap, grows on demand);
  demand paging for anonymous mmap, heap (`brk`), and file-backed mmap; CoW fork for
  code/data region; `madvise(MADV_DONTNEED)` to release physical frames. All protected by
  `spinlock_irqsave` for SMP safety. See [memory.md](memory.md).

- **SMP**: Detects CPUs via ACPI MADT; sends INIT-SIPI-SIPI to bring APs to 64-bit mode; each
  CPU has a `cpu_t` reachable via `gs:0`. IPI TLB shootdown and panic halt. See [smp.md](smp.md).

- **Interrupts**: LAPIC (per-CPU timer at 10 ms, TSC-calibrated via PIT channel 2) + IOAPIC
  (keyboard IRQ1, ATA IRQ14) + IDT with 256 entries.

- **Locking**: Ticket spinlock (`spinlock_t`) with `spinlock_irqsave`/`spinlock_irqrestore`
  variants protecting PMM, heap, VMM, and scheduler data structures.

- **Scheduler**: Per-CPU FIFO run queues; LAPIC timer preemption; work-stealing when a CPU goes
  idle; IPI kick when a remote queue receives a new thread; deferred-activation protocol for safe
  fork isolation. See [scheduler.md](scheduler.md).

- **User Threads**: `SYS_clone` with `CLONE_VM|CLONE_FILES|CLONE_THREAD|CLONE_SETTLS` creates
  threads sharing fd_table, heap, mmap, and cwd via a reference-counted `thread_group_t`.
  `SYS_futex` (FUTEX_WAIT/FUTEX_WAKE) enables mlibc pthreads. `SYS_arch_prctl` sets FS base
  for TLS.

- **Storage**: GPT partition parser + block device registry (`/dev/sda`, `/dev/sda1`, `/dev/sda2`);
  64 KiB CLOCK write-through block cache; ATA DMA driver (PCI bus-master, 4 KiB bounce buffer);
  VFS with filesystem type registry (up to 16 types) and mount table (up to 16 mounts).
  Filesystem drivers: ext2 (read/write, symlinks, mknod, chmod, mkdir, rename, unlink),
  tmpfs (in-memory), sysfs (live-callback read-only), fat32 (read-only, LFN + 8.3). See
  [storage.md](storage.md) and [fs-driver-api.md](fs-driver-api.md).

- **Terminal**: GOP linear framebuffer console (1280×800, 32 bpp, 160×50 char grid,
  LatArCyrHeb-16 PSF2 font, blinking cursor); VGA 80×25 text-mode fallback; full UTF-8
  state machine with PSF2 Unicode table lookup; VT/ANSI CSI escape parser; TTY line
  discipline (canonical/raw mode, echo, SIGINT on ^C, termios ioctls). See [tty.md](tty.md).

- **Networking**: virtio-net PCI legacy driver (primary; vendor `0x1AF4`/device `0x1000`,
  split virtqueue, `VNET_QUEUE_SIZE=256`, 3 contiguous pages per queue) + RTL8139 driver
  (fallback); lwIP UDP/TCP stack; POSIX socket syscalls; AF_UNIX SOCK_STREAM sockets (static
  pool, 4 KiB ring buffer); `/sys/class/net/eth0/` sysfs live counters. Userspace DNS resolver
  (RFC 1035 A-record, CNAME following). Userspace tools: `ping`, `nc`, `wget`, `ifconfig`,
  `udp_echo`, `tcp_connect`. See [networking.md](networking.md).

- **IPC**: Pipes (64 KiB ring buffer, `O_CLOEXEC`), AF_UNIX SOCK_STREAM, POSIX signals (32
  standard signals, `sigaction`/`sigprocmask`/`kill`/`sigreturn`).

- **Userspace**: Ring-3 ELF processes; POSIX process model (fork/exec/wait/exit/signals/pipes/
  working-directory); mlibc C standard library (statically linked, `x86_64-unknown-miniOS`
  target); Busybox for shell + applets; `libminiOS.a` for miniOS-specific stubs (mount, umount,
  DNS resolver). See [userspace.md](userspace.md).

## Documentation Pages

| Page | Topic |
|------|-------|
| [memory.md](memory.md) | PMM, VMM, virtual address space map, demand paging, CoW fork |
| [scheduler.md](scheduler.md) | Per-CPU scheduling, work-stealing, context switching, threads |
| [smp.md](smp.md) | SMP boot, per-CPU state, spinlocks, IPI |
| [storage.md](storage.md) | ATA driver, block cache, GPT, ext2, tmpfs, sysfs, fat32, VFS |
| [tty.md](tty.md) | GOP framebuffer, VT/ANSI terminal core, TTY line discipline, termios |
| [userspace.md](userspace.md) | Ring-3 entry, ELF loader, syscalls, mlibc, Busybox |
| [fs-driver-api.md](fs-driver-api.md) | Filesystem driver API (`vfs_ops_t`) |
| [networking.md](networking.md) | virtio-net driver, lwIP port, socket layer, observability |
| [known-issues.md](known-issues.md) | Known limitations and hobby OS characteristics |
