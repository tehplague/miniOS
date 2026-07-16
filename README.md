# miniOS

A small x86_64 hobby kernel with SMP, a round-robin scheduler, ext2/tmpfs/sysfs-backed virtual filesystems, lwIP networking over a virtio-net NIC, and a minimal userspace built against mlibc.

See [docs/known-issues.md](docs/known-issues.md) for a full list of known limitations and hobby OS characteristics.

## Prerequisites

**Required:**

| Tool | Purpose |
|------|---------|
| `clang` + `llvm` + `lld` | Cross-compiler and linker for kernel and userspace (`clang --target=x86_64-elf`, `ld.lld`, `llvm-ar`, `llvm-ranlib`, `llvm-strip`) |
| `nasm` | Assembles `.asm` sources (trampoline, syscall entry, IDT stubs) |
| `grub-mkrescue` | Produces the bootable ISO |
| `mke2fs` | Builds the ext2 root partition |
| `mkfs.fat` | Builds the FAT32 data partition |
| `sfdisk` | Writes the GPT partition table |

**Optional:**

| Tool | Purpose |
|------|---------|
| `qemu-system-x86_64` | Run the kernel; required for `make qemu*` and functional tests |
| `gdb` | Enables the `make qemu-debug` target |
| `doxygen` | Enables the `make docs` target |
| `socat` | Required for `make udp-echo-start` (UDP echo server for network smoke tests) |

On Arch Linux / CachyOS:

```sh
pacman -S clang llvm lld nasm grub mtools dosfstools e2fsprogs util-linux qemu-system-x86
```

## Building

```sh
# 1. Build mlibc (one-time; skipped automatically if sysroot/ is already populated)
make mlibc

# 2. Build the kernel, userspace programs, ISO, and disk image
make
```

`make mlibc` clones [mlibc](https://github.com/managarm/mlibc), installs the miniOS sysdeps from `mlibc-sysdeps/miniOS/`, and builds the C library into `sysroot/`. The default `make` target builds the kernel, userspace binaries, Busybox, the bootable ISO, and the GPT disk image used by QEMU.

## Make Targets

All targets are run from the repository root.

### Artifacts

| Target | Output | Description |
|--------|--------|-------------|
| `make` | `dist/x86_64/kernel.bin` | Link the kernel ELF |
| `make iso` | `dist/x86_64/kernel.iso` | GRUB bootable ISO (stripped) |
| `make disk-img` | `dist/x86_64/disk.img` | 200 MiB GPT disk: 64 MiB ext2 root (p1) + 128 MiB FAT32 data (p2) |

The ext2 root (partition 1) is populated from `targets/x86_64/testfs/`. The FAT32 data partition (partition 2) is populated from `targets/x86_64/data/` — drop files or subdirectories there before `make disk-img` and they appear in the partition root. An empty `data/` directory (`.gitkeep` only) produces an empty partition.

### Running

| Target | SMP CPUs | Notes |
|--------|----------|-------|
| `make qemu` | 4 | Interactive QEMU session |
| `make qemu-smp1` | 1 | Single-CPU QEMU session |
| `make qemu-smp4` | 4 | Same as `make qemu`, explicit alias |
| `make qemu-debug` | 4 | QEMU monitor on stdio, `-d int,cpu_reset` |

The disk image boots into the userspace `init` process, which mounts `/sys`, `/tmp`, and `/dev`, tries `/.autorun`, and falls back to `/bin/sh`. Type `exit` to shut down.

### Testing

| Target | What it runs |
|--------|-------------|
| `make check` | Host-native Unity unit tests (PMM, heap, VFS, scheduler, IPC, networking, ...) |
| `make test-udp-echo` | QEMU smoke test: boots kernel, verifies lwIP init in serial output |
| `make test-tcp-connect` | Same for TCP connect path |
| `make test-nc` | QEMU functional test for the userspace `nc` TCP client |
| `make test-wget` | QEMU functional test for the userspace `wget` HTTP client |
| `make test-functional` | All of the above |

Functional tests boot the kernel headlessly and verify shell commands, fork/exec, signals, pipes, mmap, and SMP scheduling. Exit codes are propagated via QEMU's `isa-debug-exit` device.

### Networking

```sh
make lwip            # fetch and build lwIP (one-time; output: _lwip-build/liblwip.a)
make udp-echo-start  # start socat UDP echo server on host port 10007
make qemu-smp1       # boot with virtio-net NIC; run ping/udp_echo inside shell
make qemu-smp1 NET_DUMP=1  # same, capturing traffic to /tmp/miniOS-net.pcap
```

See [docs/networking.md](docs/networking.md) for the full stack description.

### Other

```sh
make docs        # Doxygen HTML (requires doxygen; output: docs/html/)
make clean       # Remove build artifacts
make distclean   # Full clean including dist/, build/, and fetched third-party build outputs
```

## Source Layout

```
src/
  kernel/          Core kernel (PMM, VMM, VFS, scheduler, syscalls, IPC)
  kernel/net/      Network stack (netdev, lwIP port, socket layer, observability)
  arch/x86_64/     Architecture layer (IDT, LAPIC, SMP trampoline, syscall entry)
  arch/x86_64/drivers/virtio_net.c  virtio-net PCI legacy NIC driver (primary)
  arch/x86_64/drivers/rtl8139.c    RTL8139 PCI NIC driver (fallback)
user/              Userspace binaries (init, ifconfig, nc, wget, udp_echo, tcp_connect, …)
include/miniOS/    Public headers shared by kernel and userspace
include/miniOS/net/  Network headers (netdev, lwip_netif, net_socket, net_observe)
tests/
  unit/            Host-native Unity tests
  functional/      QEMU-driven end-to-end tests (test_udp_echo.sh, test_tcp_connect.sh, test_nc.sh, test_wget.sh, …)
targets/x86_64/    Linker script, GRUB config, ISO skeleton, testfs root
  testfs/          ext2 root filesystem staging tree (prefills partition 1)
  data/            FAT32 data partition staging tree (prefills partition 2)
```

## Booting in QEMU Manually

```sh
qemu-system-x86_64 \
  -m 128 -smp 4 -no-reboot -no-shutdown \
  -boot order=d \
  -cdrom dist/x86_64/kernel.iso \
  -nic user,model=virtio-net-pci \
  -drive file=dist/x86_64/disk.img,format=raw,if=ide,index=0
```

To attach GDB, add `-s -S` and connect with:

```
(gdb) target remote :1234
(gdb) symbol-file dist/x86_64/kernel.bin
```
