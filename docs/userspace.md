# Userspace in miniOS

Userspace is the environment where user-level applications run, isolated from the kernel. In miniOS, this is a ring 3 environment with a well-defined interface for interacting with the kernel.

## Privilege Separation

- **Ring 3**: All user programs, including the shell, execute at privilege level 3 (ring 3). This prevents them from directly accessing hardware or modifying the kernel's memory, enhancing system stability.
- **Kernel Space**: The kernel runs at privilege level 0 (ring 0), with full access to the hardware.

## Syscall Interface

The syscall (system call) interface is the bridge between userspace and the kernel. User programs use it to request services that only the kernel can provide.

- **Mechanism**: Syscalls are triggered using the `SYSCALL` instruction, which transitions the CPU from ring 3 to ring 0 and transfers control to a registered kernel handler.
- **Services**: Key services include process management (`fork`, `exec`, `wait`), file operations (`open`, `read`, `write`, `close`, `mknod`, `chmod`, `access`, `rmdir`, `unlink`), socket operations (`socket`, `connect`, `sendto`, `recvfrom`, `listen`, `accept`), and memory management (`brk`, `mmap`). In total, 43 Linux-ABI compatible syscalls are implemented.
- **Implementation layout**: The public syscall entrypoint stays in `src/kernel/syscall.c`, while the implementation is grouped internally into memory-management, filesystem, process-control, and networking pieces.

## ELF Loader

The kernel includes a loader for the ELF (Executable and Linkable Format), which is the standard binary format for executables.

- **`exec`**: The `SYS_exec` syscall uses the ELF loader to load a new program into memory.
- **Functionality**: The loader reads the ELF header, validates it, and then maps the program's segments (like `.text` and `.data`) into the process's virtual address space according to the program headers.
- **Heap Management**: The loader also determines the initial end of the program's data segments, which is used to set the initial `brk` for the process's heap.

## mlibc C Standard Library

miniOS uses [mlibc](https://github.com/managarm/mlibc) as its C standard library, built for the `x86_64-unknown-miniOS` target with POSIX support enabled.

- **Functionality**: mlibc provides a full POSIX-compliant C library including `printf`, `malloc`, `fopen`, `fread`, and the full `<sys/*>` header surface required by Linux applications.
- **Sysdeps**: The miniOS-specific syscall wiring lives in `mlibc-sysdeps/miniOS/`. It connects mlibc's generic POSIX layer to the kernel's Linux-ABI syscalls (e.g. `SYS_brk`, `SYS_read`, `SYS_write`).
- **Toolchain**: A convenience wrapper `toolchain/bin/x86_64-miniOS-cc` wraps Clang with the correct `--sysroot`, `--target`, and link flags so user programs can be built like native binaries.

## Init and Shell Startup

The first user process is `user/init/main.c`. It is responsible for:

- Mounting `/sys` as `sysfs`
- Mounting `/tmp` and `/dev` as `tmpfs`
- Attempting to execute `/.autorun`
- Falling back to `/bin/sh` if no autorun script is present

## Interactive Shell

The primary user interface for miniOS is a simple, interactive shell that runs as the first user process. It demonstrates the full userspace stack by allowing the user to:
- Type commands.
- Execute programs from the filesystem (`exec`).
- Create child processes (`fork`).
- Wait for programs to complete (`wait`).

## Terminal Interface

miniOS includes a VT/ANSI terminal stack that gives userspace a real terminal
device rather than a raw VGA write port:

### VT Core (`vt.h`)

The VT core (`src/arch/x86_64/drivers/vt.c`) is a state machine that parses ANSI/VT100
escape sequences and renders output to either the GOP linear framebuffer (1280×800, 160×50
character grid, LatArCyrHeb-16 PSF2 8×16 font, blinking underline cursor) when GRUB provides
a Multiboot2 framebuffer tag, or the VGA 80×25 text-mode framebuffer as fallback. A full
UTF-8 decoder is integrated: multi-byte sequences (2–4 bytes) are decoded and looked up in
the PSF2 Unicode table (binary search).

- **Cursor movement**: CSI A/B/C/D (up/down/forward/back), CSI H (absolute position),
  CSI 2J (erase screen), CSI K (erase to end of line).
- **Colors**: CSI m SGR sequences. 256-color requests (38;5;N) map to the nearest
  VGA 16-color palette entry using squared RGB distance for deterministic behavior
  on VGA hardware.
- **Hardware cursor**: Synced to software cursor position via VGA CRT registers
  (port 0x3D4/0x3D5) so the physical underline cursor tracks VT output.

### TTY Line Discipline (`tty.h`)

The TTY layer sits above the VT core and below the VFS character device:

- **Canonical mode** (`ICANON`): buffers input until newline or VEOF (^D). Supports
  backspace editing in the line buffer with echo.
- **Raw mode** (`ICANON` cleared): passes each character immediately.
- **Echo** (`ECHO` flag): feeds each input character back through `vt_write_byte()`.
- **Signal generation** (`ISIG` flag): sends `SIGINT` to the foreground process group
  on VINTR (^C).
- **Ioctls**: `TCGETS`/`TCSETS` read/write the `struct minios_termios`; `TIOCGWINSZ`
  returns live grid dimensions via `vt_get_winsize()` — 160×50 in GOP mode (1280×800 /
  8×16 font), 80×25 in VGA text mode.

### `/dev/tty` and `/dev/null`

The TTY is exposed as a VFS character device. The `init` process creates the device nodes `/dev/tty` and `/dev/null` at boot time using the `mknod` syscall. The kernel's VFS layer then dispatches file operations on these nodes to the appropriate driver (TTY or a null device handler).

### Shell Integration

The shell sets `TERM=xterm-256color` in its environment. It calls `TCGETS` on
startup and uses `TCSETSW` to switch between canonical mode (for the prompt) and
raw mode (for line editing). This gives the shell proper terminal semantics without
any host-side TTY emulation.

## Signal Delivery

miniOS implements 32 POSIX signals (signals 1–31, matching Linux signal numbers).

### Dispatch point

`signal_dispatch()` runs at the end of every syscall return path. It scans the current thread's `pending_signals` bitmask and processes each set bit in order.

| Handler | Action |
|---------|--------|
| `SIG_DFL` | Terminates thread (exit code 128+sig) for all signals except `SIGCHLD` (ignored by default) |
| `SIG_IGN` | Pending bit cleared, signal discarded |
| `SIGKILL` | Always fatal; ignores action table; closes sockets then exits |
| Custom `sighandler_t` | `ring3_invoke_handler()` redirects SYSRET to handler |

### Ring-3 handler invocation (`ring3_invoke_handler`)

1. **Snapshot** pre-signal state into `signal_ctx_*` fields — including both `signal_ctx_rip` and `signal_ctx_rsp` — so `SYS_sigreturn` can restore the full execution point and any in-progress SA_RESTART context.
2. **Align RSP** — `saved_user_rsp` is rounded down to a 16-byte boundary then decremented by 8. This places RSP in the same state as after a `call` instruction from a 16-byte-aligned stack, which is what the SysV x86-64 ABI requires at function entry. SSE/AVX locals in the handler (`-0x40(%rbp)`, etc.) would fault on misaligned stacks without this step.
3. **Push return address** — writes the signal trampoline VA (or, if none registered, the interrupted RIP) onto the user stack via a direct physical-memory write (kernel-side `phys + KERNEL_VMA`).
4. **Redirect SYSRET** — `saved_user_rip` is set to the handler address; `saved_user_rdi` is set to the signal number.
5. **Argument delivery** — `syscall.asm` checks `saved_user_rdi | saved_user_rsi` before SYSRET; if nonzero, it loads `saved_user_{rdi,rsi,rdx}` into RDI/RSI/RDX, delivering the signal number in RDI per the SysV ABI.

### Return and SA_RESTART

If a signal trampoline VA is registered (via a custom `sigaction` path), the handler returns through it, which invokes `SYS_sigreturn`. That syscall restores both `signal_ctx_rip` → `saved_user_rip` and `signal_ctx_rsp` → `saved_user_rsp`, resuming the thread at the original RIP and RSP (or, if `SA_RESTART` was set and a restartable syscall was in progress, re-issuing it). RSP restoration is essential because the alignment adjustment in step 2 shifts the handler frame relative to the original stack; without it, user RSP would drift by 8 bytes per signal.

### Exception-triggered termination

Faulting user-space instructions generate CPU exceptions that also terminate the thread rather than halting the kernel:

- **`#PF` (page fault)**: user-mode fault bit (error code bit 2) set → `sched_exit_current(11)`.
- **`#GP` (general protection)**: CS selector == `USER_CS` (0x23) → full register dump to serial console, then `sched_exit_current(-11)`. Kernel-mode `#GP` calls `ipi_panic_halt()`.

---

## Current Userspace Tooling

The root filesystem now combines Busybox applets with miniOS-native binaries:

- Busybox provides common shell applets such as `sh`, `ls`, `cat`, `echo`, `mkdir`, `rm`, and `ping`
- miniOS-native binaries currently include `init`, `ifconfig`, `nc`, `wget`, `udp_echo`, `tcp_connect`, `rawtest`, and several test programs

- **`nc`**: TCP client mode only. Bridges stdin/stdout to a connected socket for simple transfer and smoke-test workflows.
- **`wget`**: IPv4 HTTP GET only. It prints the raw HTTP response body, without HTTPS, redirects, or DNS support.
- **Busybox compatibility layer**: `busybox-compat/` provides the small set of headers mlibc does not install (`features.h`, `malloc.h`, `paths.h`, `sys/ioctl.h`, `sys/statfs.h`, `sys/sysmacros.h`, `sys/cpuset.h`, `mntent.h`). `libminiOS.a` provides `mount`/`umount` and `mntent` implementations not yet in mlibc-sysdeps.
