# Memory Management in miniOS

miniOS features a complete physical and virtual memory management system designed for the x86_64 architecture. This document provides an overview of its key components and design decisions.

## Physical Memory Management (PMM)

The Physical Memory Manager, or frame allocator, is responsible for managing the system's physical memory frames.

- **Initialization**: The PMM is initialized using the memory map provided by the GRUB Multiboot 2 bootloader. It identifies available RAM and creates a bitmap allocator to manage free frames. The bitmap is placed immediately after the kernel image in physical RAM and sized to cover only detected physical addresses (~2 KiB for a 64 MiB VM).
- **Single-frame allocation** (`pmm_alloc_frame`): linear scan of the bitmap for the first non-zero byte; O(n) worst case, O(1) amortised when the free list stays near the low end. Returns a 4 KiB-aligned physical address, or 0 on OOM.
- **Contiguous multi-frame allocation** (`pmm_alloc_contiguous(n)`): linear bitmap scan for the first run of `n` consecutive free frames. O(n). Intended for infrequent DMA allocations (virtio virtqueues, DMA ring buffers) where physically contiguous memory is required. `pmm_free_contiguous(phys, n)` mirrors it.
- **Free** (`pmm_free_frame`): sets the bitmap bit, increments `total_free`. Double-free guarded.

## Virtual Memory Management (VMM)

The Virtual Memory Manager handles the virtual address space for the kernel and user processes.

- **Higher-Half Kernel**: The kernel is relocated to the higher half of the 64-bit virtual address space, starting at `KERNEL_VMA = 0xFFFF800000000000`. This separates the kernel's address space from userspace, which resides in the lower half.
- **Paging**: A four-level paging structure (PML4, PDPT, PDT, PT) is used to translate virtual to physical addresses.
- **Identity Mapping**: An initial identity map is created to manage the transition into long mode. This map is torn down shortly after, forcing all kernel code to use the correct higher-half addresses and preventing subtle bugs.
- **Kernel Heap**: A boundary-tag heap is implemented for dynamic memory allocation within the kernel. It resides in its own dedicated virtual address range starting at `0xFFFF820000000000` (PML4 slot 260), separate from the kernel's code and data sections.
- **Page Allocator**: The VMM includes a page allocator for managing virtual pages.
- **MMIO Mapping**: Memory-mapped I/O regions, such as the LAPIC at `0xFEE00000`, are mapped into the kernel's address space using dedicated page tables to ensure access.

## Virtual Address Space Map

The 64-bit canonical address space is split at the 48-bit boundary. Addresses `0x0000800000000000`–`0xFFFF7FFFFFFFFFFF` are non-canonical (hardware fault on access).

```
Virtual Address Space (x86_64, 48-bit canonical)
═══════════════════════════════════════════════════════════════════

0xFFFFFFFFFFFFFFFF ┐
                   │  (unmapped / reserved)
0xFFFF830001000000 ┘

0xFFFF830000000000 ──  DMA bounce buffer        (PML4 slot 262)
                       1 page (4 KiB) for ATA PRDT / bounce I/O

0xFFFF824000000000 ──  Kernel heap limit (HEAP_MAX = HEAP_START + 64 MiB)
                   │
                   │  Kernel heap — grows upward on demand
                   │  boundary-tag allocator, 64 MiB cap
                   │
0xFFFF820000000000 ──  Kernel heap base (HEAP_START) (PML4 slot 260)

0xFFFF80FEE01000   ──  LAPIC MMIO end
0xFFFF80FEE00000   ──  LAPIC base (mapped 1:1 from phys 0xFEE00000)

0xFFFF8000000B9000 ──  VGA text buffer end
0xFFFF8000000B8000 ──  VGA text buffer  (phys 0xB8000, 4 KiB)

0xFFFF800000100000 ──  Kernel load address (KERNEL_VMA + 1 MiB)
                   │  .text  .rodata  .data  .bss
                   │  (exact extents set by linker.ld)
                   │
0xFFFF800000000000 ──  KERNEL_VMA — kernel higher-half base (PML4 slot 256)

═══════════════════════════════════════════════════════════════════
                   (non-canonical hole — hardware fault on access)
═══════════════════════════════════════════════════════════════════

0x00007FFFFFFFFFFF ──  User space ceiling

                   │  User stack   (grows downward from ceiling)
                   │
                   │  mmap / shared libraries
                   │
                   │  User heap    (grows upward via brk/sbrk)
                   │
                   │  BSS / data / text of loaded ELF
                   │
0x0000000000400000 ──  Typical ELF load address

0x0000000000001000 ──  (first usable page — 0x0 kept unmapped to catch NULL derefs)
0x0000000000000000 ┘
```

### Key Constants (defined in `vmm.h`)

| Symbol | Value | Purpose |
|---|---|---|
| `KERNEL_VMA` | `0xFFFF800000000000` | Higher-half kernel base (PML4 slot 256) |
| `VGA_BUFFER_VA` | `0xFFFF8000000B8000` | VGA text-mode framebuffer |
| `HEAP_START` | `0xFFFF820000000000` | Kernel heap base (PML4 slot 260) |
| `HEAP_MAX` | `HEAP_START + 64 MiB` | Kernel heap ceiling |
| `DMA_BUFFER_VA` | `0xFFFF830000000000` | ATA DMA bounce buffer (PML4 slot 262) |
| `PAGE_SIZE` | `0x1000` | 4 KiB page |

## Key Virtual Memory Areas

- **Kernel Code/Data**: `0xFFFF800000000000` and onwards.
- **Kernel Heap**: Starts at `0xFFFF820000000000`.
- **DMA Bounce Buffer**: A fixed virtual address at `0xFFFF830000000000` is reserved for DMA operations, simplifying physical address translation for devices like the ATA controller.

The canonical source for all virtual memory address constants is `vmm.h`, ensuring consistency across the kernel.

## SMP Memory Safety: Spinlock Protection

All memory allocators are protected by ticket spinlocks for SMP correctness:

- **PMM** (`pmm_alloc_frame`, `pmm_free_frame`): guarded by `pmm_lock` via
  `spinlock_irqsave`. Because the LAPIC timer ISR can call the scheduler which
  calls `pmm_alloc_frame`, IRQ-save locking is required on all CPUs.
- **Kernel heap** (`kmalloc`, `kfree`): guarded by `heap_lock` via
  `spinlock_irqsave`. The heap is shared across all CPUs.
- **VMM** (`vmm_map_page`, `vmm_unmap_page`): guarded by `vmm_lock`. After
  unmapping a page, `ipi_tlb_shootdown()` broadcasts a TLB shootdown IPI to
  ensure all CPUs flush the stale TLB entry before the caller continues.

The `spinlock_irqsave` variant disables local interrupts before acquiring,
preventing a deadlock where the LAPIC timer ISR on the same CPU tries to
re-acquire a lock already held by the interrupted kernel path.

## `mprotect` — Changing Page Permissions at Runtime

`mprotect(addr, len, prot)` (SYS_mprotect = 10) updates the PTE flags for
every mapped page in the range `[addr, addr + len)`:

- `addr` must be page-aligned; unaligned addresses return `EINVAL`.
- Pages in the range that are not mapped are silently skipped.
- `PROT_WRITE` sets `PAGE_WRITE` in the PTE; `PROT_READ` and `PROT_NONE`
  both produce a read-only mapping (`PAGE_PRESENT | PAGE_USER` only). Because
  miniOS does not enable the NX bit in PTEs, execute permission cannot be
  revoked — `PROT_EXEC` has no effect.
- Each remapped page triggers a local `invlpg` (via `vmm_map_page`) and an
  `ipi_tlb_shootdown` IPI so all CPUs flush the stale TLB entry.
- The `prot` field of any `mmap_region_t` that covers the range is updated
  to reflect the new protection.

The mlibc sysdep `Sysdeps<VmProtect>` is implemented and routes `mprotect(3)`
calls directly to `SYS_mprotect`.

## Demand Paging

miniOS implements three categories of demand-paged (lazy) memory:

### Anonymous mmap (`MAP_ANONYMOUS | MAP_PRIVATE`)

`sys_mmap` records the region in `mmap_regions[]` with `lazy = 1` and
returns the base address without allocating any physical pages. On the first
write or read to any page in the region the CPU generates a `#PF`. The page
fault handler calls `vmm_resolve_user_fault()`, which allocates a zeroed
physical frame and installs the PTE with permissions derived from `region.prot`.
Subsequent accesses to the same page hit the TLB/page table normally.

### Heap (`brk`)

`sys_brk` simply advances `*brk_ptr` to the requested address without
allocating pages. A `brk_base` field (set at ELF load time, inherited at
fork/clone) marks the lower bound of the heap. Any `#PF` in
`[brk_base, *brk_ptr)` is handled by `vmm_resolve_user_fault()`, which
allocates a zeroed read/write page. The shrink path (`new_brk < old_brk`)
still walks the existing PTEs and frees any already-mapped frames, so memory
is correctly reclaimed.

### File-backed mmap (`MAP_PRIVATE`, fd ≥ 3)

`sys_mmap` records the file's inode, file offset, file size and `vfs_ops_t *`
pointer in the region slot with `lazy = 1`. No I/O is performed at mmap time.
On `#PF`, `vmm_resolve_user_fault()` computes the file offset for the faulting
page (`region.file_offset + page_index * PAGE_SIZE`), calls
`ops->read(inode, offset, kva, PAGE_SIZE)` to populate the frame, zero-pads
any partial trailing page, and installs the PTE. The `vfs_ops_t *` pointer is
a stable static kernel pointer (e.g. `&g_ext2_vfs_ops`) so it remains valid
even if the fd is closed after mmap.

### SMP safety

`vmm_resolve_user_fault()` checks `vmm_virt_to_phys(page_va) != 0` before
allocating to handle the race where two CPUs fault the same page simultaneously.
The second CPU finds the page already present and returns 0 immediately.
`vmm_map_page()` issues both a local `invlpg` and `ipi_tlb_shootdown()` to
ensure all CPUs see the new mapping.

## Copy-on-Write (CoW) for Fork

Every process has its own PML4 (`vmm_new_address_space()`) — see `docs/scheduler.md`
for the full per-process address space and fork protocol. Code/data/BSS/heap and
mmap-region pages are shared between parent and child via `fork_cow_share_page()`
(`src/kernel/sched/sched.c`) instead of eager copying:

- **Read-only pages** (`.text`/`.rodata`, or a page already CoW'd from an earlier fork):
  mapped into the child's own PML4 with the same permissions — shared directly, never
  written without going through the CoW fault path first.
- **Writable pages**: *both* the parent's own mapping and the child's new mapping have
  `PAGE_WRITE` cleared and `PAGE_COW` set (bit 9, a software-reserved PTE bit on
  x86-64). On the first write by **either side**, the CPU raises a protection fault
  (error code bit 0). `vmm_resolve_user_fault()` detects `PAGE_COW`, allocates a fresh
  frame, copies the content via the `KERNEL_VMA` alias, remaps that process's own VA
  writable with `PAGE_COW` cleared, and drops its share of the original frame
  (`pmm_unref_frame()`).

Every shared frame carries a refcount (`pmm_ref_frame()`/`pmm_unref_frame()`,
`src/kernel/mm/pmm.c`) — incremented once per `fork_cow_share_page()` call, decremented
whenever a process stops using it (first CoW write, process exit, or `execve()`). The
frame is only actually returned to the free pool when the count reaches zero, so a
frame shared across several fork generations is freed exactly when the last owner lets
go, regardless of write order between them.

**Stack pages are not CoW** — eagerly copied at fork time via the `KERNEL_VMA`
direct-map alias (parent and child both already have their own address space by the
time `sched_fork()` copies the content, so there's no shared-VA hazard to avoid; stack
pages are just few enough that CoW isn't worth the complexity).

**Exit / exec correctness.** `vmm_free_address_space()` (process exit) and
`sys_execve()`'s image teardown both just call `pmm_unref_frame()` on every mapped page
in the process's own low half — no fork-lineage bookkeeping needed, since the refcount
already tracks how many processes still reference a given frame.

## `madvise(MADV_DONTNEED)`

`madvise(addr, len, MADV_DONTNEED)` (SYS_madvise = 28, advice value 4):

1. Every mapped page in `[addr, addr+len)` is unmapped and its physical frame
   returned to the PMM.
2. Any `mmap_region_t` overlapping the range has `lazy = 1` set, so the next access
   triggers a fresh demand-page fault (zeroed frame for anonymous; file content
   re-read for file-backed).

All other advice values return `EINVAL`. This lets mlibc's `free()` return physical
pages to the OS rather than keeping freed heap pages mapped indefinitely.

## Address Space Layout Randomisation (ASLR)

Two regions are randomised per `exec` using low bits of `rdtsc()`:

| Region | Randomised? | Entropy | Constraint |
|--------|-------------|---------|------------|
| mmap base | Yes | 8 bits (256 pages = 1 MiB) | `MMAP_BASE - rand * PAGE_SIZE` |
| Stack RSP | Yes | 8 bits (256 slots × 16 B) | displacement below `USER_STACK_TOP` |
| Heap base (`brk_base`) | No | — | fixed at ELF image end |
| Text (code) | No | — | fixed at `0x400000` |

Randomisation happens in `sched_create_user_task()` (initial task creation) and again in
`sys_execve()` for non-fork execs. Fork children inherit the parent's layout unchanged
(correct POSIX semantics — child addresses must match parent until `exec`).

Heap and text randomisation require position-independent executables (PIE), which miniOS
does not yet support (all binaries link at fixed address `0x400000`).
