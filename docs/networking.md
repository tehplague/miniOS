# Networking in miniOS

miniOS implements a full UDP/TCP network stack using [lwIP](https://savannah.nongnu.org/projects/lwip/)
as the protocol engine, backed by a virtio-net NIC driver (RTL8139 as fallback) and exposed
to userspace via POSIX-like socket syscalls.

## Architecture

```
Userspace (ring 3)
  udp_echo / tcp_connect / rawtest / ifconfig / nc / wget / ping
       |
  socket / bind / connect / sendto / recvfrom (syscalls)
       |
  Kernel socket layer  (net_socket.c)
  net_sock_t pool, lwIP PCB binding, vfs_file_t fd extension
       |
  lwIP stack  (liblwip.a)
  netif glue: lwip_netif.c / lwip_port.c
       |
  netdev abstraction  (netdev.c)
  net_device_t ops: open / poll / xmit / rx_handler
       |
  virtio-net PCI legacy driver  (virtio_net.c)   ← primary
  RTL8139 PCI driver            (rtl8139.c)       ← fallback
       |
  QEMU -nic user,model=virtio-net-pci  (SLIRP user networking)
```

## Layers

### virtio-net Driver (`src/arch/x86_64/drivers/virtio_net.c`)

Primary NIC driver, probed before RTL8139. Uses the virtio PCI legacy interface.

- **PCI probe**: scans for vendor `0x1AF4` / device `0x1000` (transitional virtio-net).
  BAR0 must be an I/O bar (bit 0 set). Bus-master DMA enabled via PCI command register.
- **Feature negotiation**: accepts only `VIRTIO_NET_F_MAC` (bit 5). All offload features
  (GSO, checksum) are declined so each TX descriptor carries a zeroed `virtio_net_hdr`.
- **Virtqueue layout** (legacy split virtqueue, `VNET_QUEUE_SIZE = 256`):
  - Desc table at PFN×4096 + 0 (4096 bytes, one page).
  - Available ring at PFN×4096 + 4096 (520 bytes, partial page 1).
  - Used ring at PFN×4096 + 8192 (2054 bytes, partial page 2).
  - Total: 3 physically contiguous pages per queue (`pmm_alloc_contiguous(3)`).
  - **Queue size pitfall**: the device-reported `QueueSize` is read-only in legacy mode;
    the driver must use it exactly or the ring offsets land in wrong memory. QEMU reports
    256; `VNET_QUEUE_SIZE` asserts this at init.
- **RX queue (queue 0)**: `VNET_RX_SLOTS = 64` descriptors pre-filled at init, each
  pointing to a BSS-static `g_rx_bufs[64][1528]` buffer. `poll()` drains the used ring,
  passes frames (minus the 10-byte `virtio_net_hdr`) to the lwIP rx_handler, and
  refills each processed slot.
- **TX queue (queue 1)**: single BSS-static `g_tx_buf` prefixed with a zeroed
  `virtio_net_hdr`. `xmit()` fills one descriptor, updates the available ring, kicks
  queue 1, then busy-waits for `used.idx` to advance (synchronous TX).
- **Memory barrier**: `__asm__ volatile("" ::: "memory")` compiler barriers surround
  ring index writes before I/O kicks. x86 TSO guarantees store–store order; the port
  I/O write that kicks the queue acts as a serialising instruction.

### RTL8139 Driver (`src/arch/x86_64/drivers/rtl8139.c`)

Drives the emulated Realtek RTL8139 PCI NIC that QEMU exposes.

- **PCI init**: Scans the PCI bus for vendor `0x10EC` / device `0x8139`; enables bus-master
  DMA via the PCI command register (`cmd | 0x04` — required for QEMU to forward DMA writes).
- **TX**: Single descriptor, polled; frames written to a BSS-static DMA buffer whose physical
  address is `virt − KERNEL_VMA`.
- **RX**: 8 KB ring buffer (`RCR RBLEN=0b00`). The ISR fires on `ISR_ROK`; `ring_advance` uses
  `raw_len` (header + frame + CRC = 4 + frame_len + 4) to advance the read pointer correctly.
### Network Device Abstraction (`src/kernel/net/netdev.c`)

`netdev.h` defines `net_device_t` — a thin ops struct (`open`, `poll`, `xmit`, `rx_handler`)
that decouples the lwIP port from the NIC driver. Up to `NET_MAX_DEVICES=4` devices are
registered. `net_init()` probes virtio-net first; if no virtio-net device is found it falls
back to RTL8139. `netdev_poll_all()` drives the RX path from the dedicated network poll thread.

### lwIP Port (`src/kernel/net/lwip_port.c`, `lwip_netif.c`)

- `lwip_port.c` provides the `lwip_port_*` callbacks that lwIP calls to send/receive Ethernet
  frames; it bridges `pbuf` chains to `net_device_t`.
- `lwip_netif.c` initialises a single `netif` with a static IP (`10.0.2.15/24`, gw `10.0.2.2`)
  matching QEMU's SLIRP address space and calls `lwip_init()` / `netif_add()` / `netif_set_up()`.
- `lwip_netif_init()` is called from `kernel_main` after `net_init()` and `net_observe_init()`.
- A brief RX flush loop (`netdev_poll_all()` × 2000) drains NIC buffers before userspace starts.

### Network Observability (`src/kernel/net/net_observe.c`)

Registers sysfs files under `/sys/class/net/eth0/`:

| File | Content |
|------|---------|
| `address` | MAC address (hex) |
| `link_state` | `up` or `down` |
| `rx_packets` | received frame count |
| `tx_packets` | transmitted frame count |
| `rx_dropped` | frames dropped on RX |
| `tx_dropped` | frames dropped on TX |

All files use `sysfs_create_file_show()` — their content is computed live by a callback each
time a userspace `read()` is issued.

### Kernel Socket Layer (`src/kernel/net/net_socket.c`)

A static pool of `net_sock_t` structs bridges POSIX socket semantics to lwIP PCBs.

- `net_socket_alloc()` / `net_socket_free()` manage the pool.
- Each `net_sock_t` holds a `void *pcb` (lwIP UDP or TCP PCB), type (`SOCK_DGRAM` /
  `SOCK_STREAM`), and state flags.
- `vfs_file_t` was extended with a `sock` pointer so socket fds are tracked in the VFS fd
  table alongside regular files.

### Socket Syscalls (`src/kernel/syscall_net.c`)

Seven syscalls are wired through `syscall_dispatch` → `syscall_dispatch_net()`:

| Syscall | Number | Notes |
|---------|--------|-------|
| `SYS_socket` | — | allocates `net_sock_t`, returns fd |
| `SYS_bind` | — | binds UDP PCB to local port |
| `SYS_connect` | — | connects TCP PCB or sets UDP remote |
| `SYS_sendto` | — | `dest_addr` in `R10` (arg4), `addrlen` in `R8` (arg5) |
| `SYS_recvfrom` | — | copies lwIP pbuf data to userspace |
| `SYS_listen` | — | TCP listen |
| `SYS_accept` | — | TCP accept |

mlibc wires these syscalls through the standard POSIX socket API; `user/libc/syscalls.c` provides miniOS-specific stubs (`mount`, `umount`, DNS) not yet in mlibc-sysdeps.

### Userspace DNS Resolver (`user/libc/syscalls.c`)

Name resolution is implemented entirely in userspace — no kernel DNS syscall exists.

- **Configuration**: reads the nameserver address from `/etc/resolv.conf` (set to QEMU SLIRP DNS `10.0.2.3`). Port 53 is looked up via `/etc/services` through mlibc's `getservbyname`.
- **Protocol**: opens a `SOCK_DGRAM` socket, sends an RFC 1035 A-record query (transaction ID randomised), and waits up to 2 s for a reply.
- **CNAME following**: up to 8 chained CNAME records are followed within the response packet before the A-record address is extracted.
- **API**: exposes `resolve_ipv4_hostname(name, out_addr)` used by `wget` and `nc`; not a full `getaddrinfo` shim.
- **Limitations**: IPv6 (AAAA), MX, SRV, and TCP-mode DNS are not supported.

### User-Facing Networking Tools

miniOS currently exposes several networking programs in userspace:

- **`udp_echo`**: UDP echo smoke-test client
- **`tcp_connect`**: TCP connect-and-read smoke-test client
- **`rawtest`**: raw ICMP socket test program
- **`ifconfig`**: read-only view of the configured interface address and sysfs-backed counters
- **`nc`**: TCP client mode only; bridges stdin/stdout to a connected socket
- **`wget`**: IPv4 HTTP GET only; prints the raw body to stdout
- **Busybox `ping`**: fully functional — raw ICMP socket via `SOCK_RAW`, one request per second, RTT display, statistics on `^C`. `netinet/ip.h` and `netinet/ip_icmp.h` are provided by mlibc; socket libc stubs are in `libminiOS.a`.

## Build

lwIP is fetched and built as part of the top-level Makefile:

```sh
make lwip      # fetch + build liblwip.a (one-time)
make           # links liblwip.a into the kernel
```

The lwIP source is unpacked into `lwip-src/` and built into `_lwip-build/liblwip.a`.
`lwipopts.h` is the miniOS-specific configuration (NO_SYS=0, single-thread, static memory).

## Testing

### Manual (interactive QEMU)

```sh
make udp-echo-start   # start socat UDP echo on host port 10007
make qemu-smp1        # boot miniOS with virtio-net NIC
# Inside QEMU shell:
ping 8.8.8.8          # ICMP echo; verifies full TX+RX round-trip
/test/bin/udp_echo    # should print "udp_echo: PASS"
/test/bin/tcp_connect # should print "tcp_connect: PASS"

make qemu-smp1 NET_DUMP=1  # capture traffic to /tmp/miniOS-net.pcap
```

### Automated smoke tests

```sh
make test-udp-echo     # boots QEMU, checks lwIP init in serial output
make test-tcp-connect  # same, TCP variant
make test-nc           # boots QEMU and verifies the userspace nc TCP client
make test-wget         # boots QEMU and verifies the userspace wget HTTP client
make test-functional   # runs all functional tests (including above)
```

The smoke test scripts (`tests/functional/test_udp_echo.sh`,
`tests/functional/test_tcp_connect.sh`, `tests/functional/test_nc.sh`,
`tests/functional/test_wget.sh`) boot QEMU headlessly and verify the expected
networking behavior through the serial log and host-side test fixtures.

## Timing and Clock Accuracy

Correct network timing (ping intervals, poll/select timeouts) depends on two kernel primitives:

- **`SYS_gettimeofday` (syscall 96)** — implemented in `syscall_mm.c` using the TSC. BusyBox uses `gettimeofday` (not `clock_gettime`) because `CONFIG_MONOTONIC_SYSCALL` is not set; its `monotonic_us()` calls `xgettimeofday()`.
- **`SYS_poll` / `SYS_select` deadlines** — computed as TSC-based absolute deadlines (`rdtsc() + timeout_ms * miniOS_tsc_hz / 1000`) rather than LAPIC-tick counts. This is necessary because the LAPIC calibration loop runs at variable speed depending on the host/KVM configuration.

### TSC frequency calibration

`miniOS_tsc_hz` is calibrated once at boot in `lapic_timer_init()` using **PIT channel 2** (Intel 8253/8254, driven by a 1 193 182 Hz crystal oscillator). 11 932 PIT ticks correspond to exactly 10 ms; the TSC delta over that window is multiplied by 100 to obtain Hz.

## Key Constraints

- Single netif only; no runtime device enumeration.
- Static memory (lwIP `MEM_LIBC_MALLOC=0`); heap sized for a single UDP + TCP flow.
- No interrupt-driven TX; send path is synchronous (busy-wait) from the syscall context.
- SLIRP networking only (QEMU user-mode); no bridged or TAP support in the build.
- DNS resolver is userspace-only, UDP A-record queries only; no AAAA, MX, or TCP-mode DNS.
- virtio-net TX uses a single shared `g_tx_buf`; concurrent sends from multiple threads
  are not safe without external locking.
