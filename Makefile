# miniOS — handcrafted root Makefile
# Replaces the autotools build hierarchy (configure.ac + Makefile.am files).
# Run `make mlibc` once on a fresh checkout; then `make` builds everything.

# ── Toolchain ─────────────────────────────────────────────────────────────────
CC_FOR_TARGET    ?= clang --target=x86_64-elf
LD_FOR_TARGET    ?= ld.lld
AR_FOR_TARGET    ?= llvm-ar
RANLIB_FOR_TARGET ?= llvm-ranlib
STRIP_FOR_TARGET  ?= llvm-strip
CC_USER          := $(CURDIR)/toolchain/bin/x86_64-miniOS-cc
CXX_USER         := $(CURDIR)/toolchain/bin/x86_64-miniOS-c++
NASM             ?= nasm
GRUB_MKRESCUE    ?= grub-mkrescue
MKE2FS           ?= mke2fs
MKFSFAT          ?= mkfs.fat
MCOPY            ?= mcopy
QEMU             ?= qemu-system-x86_64

# ── Paths ─────────────────────────────────────────────────────────────────────
SYSROOT        := $(CURDIR)/sysroot/usr

# ── mlibc ────────────────────────────────────────────────────────────────────
MLIBC_REPO     := https://github.com/managarm/mlibc.git
MLIBC_SRC      := $(CURDIR)/_mlibc-src
MLIBC_BUILD    := $(CURDIR)/_mlibc-build
MLIBC_SYSROOT  := $(SYSROOT)/x86_64-miniOS
MLIBC_LIBC     := $(MLIBC_SYSROOT)/lib/libc.a
MLIBC_INCLUDE  := $(MLIBC_SYSROOT)/include
MLIBC_SYSDEPS  := $(CURDIR)/mlibc-sysdeps

BUSYBOX_VERSION  := 1.37.0
BUSYBOX_TAR      := busybox-$(BUSYBOX_VERSION).tar.bz2
BUSYBOX_URL      := https://busybox.net/downloads/$(BUSYBOX_TAR)
BUSYBOX_SRC      := $(CURDIR)/busybox-src
BUSYBOX_BUILD    := $(CURDIR)/_busybox-build
BUSYBOX_BIN      := $(CURDIR)/build/user/out/busybox
BUSYBOX_UPSTREAM_BUILD := $(CURDIR)/_busybox-upstream-build
BUSYBOX_UPSTREAM_BIN   := $(CURDIR)/build/user/out/busybox-upstream
BUSYBOX_COMPAT   := $(CURDIR)/busybox-compat
BUSYBOX_PATCHES  := $(sort $(wildcard $(CURDIR)/patches/busybox/*.patch))
BUSYBOX_SYMLINKS := cat clear df echo kill ls mkdir ping rm rmdir sh sleep stat \
                     ps top pstree mount mountpoint

# ── Busybox compat-library paths ──────────────────────────────────────────────
# libminiOS.a provides miniOS-specific functions not in mlibc (mount, umount).
# Installed into the mlibc sysroot so BusyBox's trylink resolves -lminiOS.
BUSYBOX_COMPAT_SRCS := user/libc/miniOS_compat.c user/libc/mntent.c
BUSYBOX_COMPAT_OBJS := build/busybox-compat/miniOS_compat.o build/busybox-compat/mntent.o
BUSYBOX_COMPAT_LIB  := $(MLIBC_SYSROOT)/lib/libminiOS.a
BUSYBOX_COMPAT_CFLAGS := -O2 -Wall --std=c2x -isystem $(BUSYBOX_COMPAT)

# ld.lld wrapper that pre-injects the miniOS sysroot lib path so BusyBox's
# trylink finds mlibc before any host libc.a.
LD_WRAPPER_DIR := $(CURDIR)/build/ld-wrappers
LD_WRAPPER     := $(LD_WRAPPER_DIR)/ld.lld

# ── lwIP ─────────────────────────────────────────────────────────────────────
LWIP_VERSION     := 2.2.1
LWIP_TAR         := lwip-$(LWIP_VERSION).zip
LWIP_URL         := https://download.savannah.nongnu.org/releases/lwip/$(LWIP_TAR)
LWIP_SRC         := $(CURDIR)/lwip-src
LWIP_BUILD       := $(CURDIR)/_lwip-build
LWIP_LIB         := $(LWIP_BUILD)/liblwip.a

LWIP_CFLAGS := -O2 -Wall -g --std=c2x -mgeneral-regs-only -mno-red-zone \
  -mcmodel=large -nostdinc -ffreestanding -fno-stack-protector \
  -I$(CURDIR)/include \
  -I$(CURDIR)/include/miniOS/net \
  -isystem $(shell $(CC_FOR_TARGET) -print-file-name=include 2>/dev/null) \
  -I$(LWIP_SRC)/src/include

LWIP_SRCS := \
  $(LWIP_SRC)/src/core/init.c \
  $(LWIP_SRC)/src/core/def.c \
  $(LWIP_SRC)/src/core/dns.c \
  $(LWIP_SRC)/src/core/inet_chksum.c \
  $(LWIP_SRC)/src/core/ip.c \
  $(LWIP_SRC)/src/core/mem.c \
  $(LWIP_SRC)/src/core/memp.c \
  $(LWIP_SRC)/src/core/netif.c \
  $(LWIP_SRC)/src/core/pbuf.c \
  $(LWIP_SRC)/src/core/raw.c \
  $(LWIP_SRC)/src/core/stats.c \
  $(LWIP_SRC)/src/core/sys.c \
  $(LWIP_SRC)/src/core/timeouts.c \
  $(LWIP_SRC)/src/core/udp.c \
  $(LWIP_SRC)/src/core/tcp.c \
  $(LWIP_SRC)/src/core/tcp_in.c \
  $(LWIP_SRC)/src/core/tcp_out.c \
  $(LWIP_SRC)/src/core/ipv4/etharp.c \
  $(LWIP_SRC)/src/core/ipv4/icmp.c \
  $(LWIP_SRC)/src/core/ipv4/ip4.c \
  $(LWIP_SRC)/src/core/ipv4/ip4_addr.c \
  $(LWIP_SRC)/src/core/ipv4/ip4_frag.c \
  $(LWIP_SRC)/src/netif/ethernet.c \
  $(LWIP_SRC)/src/api/err.c

LWIP_OBJS := $(patsubst $(LWIP_SRC)/%.c, $(LWIP_BUILD)/%.o, $(LWIP_SRCS))

# ── Kernel compile flags ──────────────────────────────────────────────────────
KERNEL_CFLAGS := -O2 -Wall -g --std=c2x -mgeneral-regs-only -mno-red-zone \
  -mcmodel=large -nostdinc -ffreestanding -fno-stack-protector \
  -DMAX_CPUS=8 -DKEYBOARD_LAYOUT_DE -DCONSOLE_GOP -I$(CURDIR)/include \
  -isystem $(shell $(CC_FOR_TARGET) -print-file-name=include 2>/dev/null) \
  -I$(LWIP_SRC)/src/include -I$(CURDIR)/include/miniOS/net

# ── Kernel link flags ─────────────────────────────────────────────────────────
KERNEL_LDFLAGS := -nostdlib -static -n -T$(CURDIR)/targets/x86_64/linker.ld

# ── Userspace compile flags ───────────────────────────────────────────────────
# Target/sysroot/crt/libc flags are owned by toolchain/bin/x86_64-miniOS-cc.
USER_CFLAGS  := -Os -Wall --std=c2x -ffunction-sections -fdata-sections -I$(CURDIR)/user -I$(BUSYBOX_COMPAT)
USER_LDFLAGS := -Wl,--gc-sections

# ── Kernel C sources ──────────────────────────────────────────────────────────
KERNEL_SRCS := \
  src/kernel/main.c \
  src/kernel/console/printk.c \
  src/kernel/console/putchar.c \
  src/kernel/fs/elf.c \
  src/kernel/fs/ext2.c \
  src/kernel/fs/fat32.c \
  src/kernel/fs/pipe.c \
  src/kernel/fs/tmpfs.c \
  src/kernel/fs/sysfs.c \
  src/kernel/fs/procfs.c \
  src/kernel/fs/vfs.c \
  src/kernel/ipi/ipi.c \
  src/kernel/mlibc/string/atoi.c \
  src/kernel/mlibc/string/isdigit.c \
  src/kernel/mlibc/string/memcmp.c \
  src/kernel/mlibc/string/memcpy.c \
  src/kernel/mlibc/string/memmove.c \
  src/kernel/mlibc/string/memset.c \
  src/kernel/mlibc/string/snprintf.c \
  src/kernel/mlibc/string/strcat.c \
  src/kernel/mlibc/string/strcpy.c \
  src/kernel/mlibc/string/strlen.c \
  src/kernel/mlibc/string/strncmp.c \
  src/kernel/mlibc/string/strncpy.c \
  src/kernel/mlibc/string/vsnprintf.c \
  src/kernel/mm/heap.c \
  src/kernel/mm/pmm.c \
  src/kernel/mm/vmm.c \
  src/kernel/qemu_exit.c \
  src/kernel/drivers/tty.c \
  src/kernel/drivers/block_layer.c \
  src/kernel/drivers/gpt.c \
  src/kernel/drivers/block_device.c \
  src/kernel/net/lwip_port.c \
  src/kernel/net/net_observe.c \
  src/kernel/net/lwip_netif.c \
  src/kernel/net/net_socket.c \
  src/kernel/net/net_config.c \
  src/kernel/net/netdev.c \
  src/kernel/net/unix_sock.c \
  src/kernel/sched/sched.c \
  src/kernel/sched/sched_balance.c \
  src/kernel/signal.c \
  src/kernel/syscall.c \
  src/kernel/syscall_mm.c \
  src/kernel/syscall_fs.c \
  src/kernel/syscall_proc.c \
  src/kernel/syscall_net.c \
  src/kernel/userspace/enter_ring3.c

KERNEL_OBJS := $(patsubst src/kernel/%.c, build/kernel/%.o, $(KERNEL_SRCS))

# ── Arch C sources ────────────────────────────────────────────────────────────
ARCH_C_SRCS := \
  src/arch/x86_64/drivers/ata.c \
  src/arch/x86_64/drivers/pci.c \
  src/arch/x86_64/drivers/rtl8139.c \
  src/arch/x86_64/drivers/virtio_net.c \
  src/arch/x86_64/drivers/console.c \
  src/arch/x86_64/drivers/keyboard.c \
  src/arch/x86_64/drivers/vt.c \
  src/arch/x86_64/interrupts/exceptions.c \
  src/arch/x86_64/interrupts/gdt.c \
  src/arch/x86_64/interrupts/idt.c \
  src/arch/x86_64/interrupts/ioapic.c \
  src/arch/x86_64/interrupts/lapic.c \
  src/arch/x86_64/smp/acpi.c \
  src/arch/x86_64/smp/per_cpu.c \
  src/arch/x86_64/smp/smp_boot.c \
  src/arch/x86_64/smp/spinlock.c

ARCH_ASM_SRCS := \
  src/arch/x86_64/boot/header.asm \
  src/arch/x86_64/boot/main64.asm \
  src/arch/x86_64/boot/main.asm \
  src/arch/x86_64/interrupts/gdt64.asm \
  src/arch/x86_64/interrupts/isr.asm \
  src/arch/x86_64/interrupts/syscall.asm \
  src/arch/x86_64/sched/context_switch.asm \
  src/arch/x86_64/sched/user_stub.asm \
  src/arch/x86_64/smp/per_cpu.asm \
  src/arch/x86_64/smp/trampoline.asm

ARCH_C_OBJS   := $(patsubst src/arch/x86_64/%.c,   build/arch/%.o,     $(ARCH_C_SRCS))
ARCH_ASM_OBJS := $(patsubst src/arch/x86_64/%.asm, build/arch/%.asm.o, $(ARCH_ASM_SRCS))
ARCH_OBJS     := $(ARCH_C_OBJS) $(ARCH_ASM_OBJS)

# ── Userspace: libc objects ───────────────────────────────────────────────────
# mlibc provides crt1.o + libc.a; no hand-rolled libc objects needed
LIBC_SRCS :=
LIBC_OBJS :=

# ── Userspace binaries ────────────────────────────────────────────────────────
# sh links at 0x400000; all others link at 0x600000
USER_CMDS := hello echo cat testwrite testmmap testmmap_file \
               ls testmountstat mkdir rm mount testidentity testsignal \
               testpipe testsyscall27 smptest testtty test_raw_mode test_winsize \
               test_sigint \
               print_env test_env \
               init \
               lspci \
               udp_echo \
               tcp_connect \
               rawtest \
               ifconfig \
               nc \
               wget \
               testunix testfutex testjobctrl

USER_BINS := build/user/out/sh $(patsubst %, build/user/out/%, $(USER_CMDS)) \
             build/user/out/testpthread build/user/out/testptrace

# ── Userspace main.c sources (explicit listing for reference) ─────────────────
USER_MAIN_SRCS := \
  user/sh/main.c \
  user/hello/main.c \
  user/echo/main.c \
  user/cat/main.c \
  user/testwrite/main.c \
  user/testmmap/main.c \
  user/testmmap_file/main.c \
  user/ls/main.c \
  user/testmountstat/main.c \
  user/mkdir/main.c \
  user/rm/main.c \
  user/mount/main.c \
  user/testidentity/main.c \
  user/testsignal/main.c \
  user/testpipe/main.c \
  user/testsyscall27/main.c \
   user/smptest/main.c \
   user/testtty/main.c \
   user/test_raw_mode/main.c \
   user/test_winsize/main.c \
   user/test_sigint/main.c \
   user/print_env/main.c \
   user/test_env/main.c \
   user/init/main.c \
   user/lspci/main.c \
   user/udp_echo/main.c \
   user/tcp_connect/main.c \
   user/rawtest/main.c \
   user/ping/main.c \
   user/ifconfig/main.c \
   user/nc/main.c \
   user/wget/main.c

# ── Default target ────────────────────────────────────────────────────────────
.PHONY: all iso disk-img testfs-install qemu qemu-smp1 qemu-smp4 qemu-debug \
        check check-functional test-functional test-terminal test-env mlibc busybox busybox-upstream lwip docs clean \
        udp-echo-start udp-echo-stop tcp-echo-start tcp-echo-stop \
        test-raw-socket test-nc test-wget \
        test-busybox-compat test-busybox-upstream

all: iso disk-img


# ── mlibc ────────────────────────────────────────────────────────────────────
mlibc:
	@if [ ! -d "$(MLIBC_SRC)" ]; then \
	  echo "Fetching mlibc..." && \
	  git clone --depth=1 $(MLIBC_REPO) $(MLIBC_SRC); \
	fi
	@echo "Installing miniOS sysdeps into mlibc source tree..."
	@chmod -Rf u+w $(MLIBC_SRC)/sysdeps/miniOS 2>/dev/null || true
	@cp -rT $(MLIBC_SYSDEPS)/miniOS $(MLIBC_SRC)/sysdeps/miniOS
	@echo "Symlinking Linux ABI bits for miniOS..."
	@mkdir -p $(MLIBC_SRC)/sysdeps/miniOS/include/abi-bits
	@for f in $$(ls $(MLIBC_SRC)/abis/linux/); do \
	  ln -sf ../../../../abis/linux/$$f \
	    $(MLIBC_SRC)/sysdeps/miniOS/include/abi-bits/$$f; \
	done
	@echo "Patching mlibc meson.build for miniOS system..."
	@chmod u+w $(MLIBC_SRC)/meson.build
	@python3 -c "\
import pathlib; \
f = pathlib.Path('$(MLIBC_SRC)/meson.build'); \
t = f.read_text(); \
t = t.replace( \
  '# ANCHOR: demo-sysdeps', \
  \"elif host_machine.system() == 'miniOS'\n\tlibc_include_dirs += include_directories('sysdeps/miniOS/include')\n\tsubdir('sysdeps/miniOS')\n# ANCHOR: demo-sysdeps\" \
) if \"'miniOS'\" not in t else t; \
f.write_text(t)"
	@echo "Configuring mlibc with Meson (target: x86_64-unknown-miniOS)..."
	meson setup $(MLIBC_BUILD) $(MLIBC_SRC) \
	  --cross-file $(MLIBC_SYSDEPS)/options.ini \
	  --prefix $(MLIBC_SYSROOT) \
	  --default-library=static \
	  -Dbuild_tests=false \
	  -Dlibgcc_dependency=false \
	  -Dposix_option=enabled \
	  -Dlinux_option=disabled \
	  -Dglibc_option=disabled \
	  -Dbsd_option=disabled
	ninja -C $(MLIBC_BUILD)
	ninja -C $(MLIBC_BUILD) install
	@echo "Patching installed mlibc headers for miniOS..."
	@printf '\n#ifndef __MLIBC_ABI_ONLY\nstatic __inline__ void *mempcpy(void *__dest, const void *__src, __SIZE_TYPE__ __n) {\n    return (__builtin_memcpy(__dest, __src, __n), (char *)__dest + __n);\n}\n#endif\n' \
	    >> $(MLIBC_SYSROOT)/include/string.h
	@printf '\n#ifndef __MLIBC_ABI_ONLY\n#include <bits/cpu_set.h>\nint sched_getaffinity(pid_t __pid, size_t __cpusetsize, cpu_set_t *__mask);\nint sched_setaffinity(pid_t __pid, size_t __cpusetsize, const cpu_set_t *__mask);\n#endif\n' \
	    >> $(MLIBC_SYSROOT)/include/sched.h
	cp busybox-compat/sys/ioctl.h $(MLIBC_SYSROOT)/include/sys/ioctl.h
	@echo "mlibc installed to $(MLIBC_SYSROOT)."
	@chmod -R a-w $(MLIBC_SRC) && echo "$(MLIBC_SRC) set read-only — edit mlibc-sysdeps/ instead"

# ── Busybox compat library ────────────────────────────────────────────────────
build/busybox-compat/%.o: user/libc/%.c | $(MLIBC_LIBC)
	@mkdir -p $(dir $@)
	$(CC_USER) $(BUSYBOX_COMPAT_CFLAGS) -c -o $@ $<

$(BUSYBOX_COMPAT_LIB): $(BUSYBOX_COMPAT_OBJS) | $(MLIBC_LIBC)
	llvm-ar rcs $@ $^
	cp busybox-compat/mntent.h $(MLIBC_INCLUDE)/mntent.h

$(LD_WRAPPER): | $(MLIBC_LIBC)
	@mkdir -p $(LD_WRAPPER_DIR)
	@printf '#!/bin/sh\nexec /usr/bin/ld.lld -L%s/lib "$$@"\n' \
	    "$(MLIBC_SYSROOT)" > $@
	@chmod +x $@

# ── Busybox ───────────────────────────────────────────────────────────────────
$(BUSYBOX_BIN): busybox.config $(BUSYBOX_COMPAT_LIB) $(LD_WRAPPER) $(MLIBC_LIBC)
	@if [ ! -d "$(BUSYBOX_SRC)" ]; then \
	  echo "Fetching Busybox $(BUSYBOX_VERSION)..." && \
	  wget --show-progress -O $(BUSYBOX_TAR) $(BUSYBOX_URL) && \
	  tar -xjf $(BUSYBOX_TAR) && \
	  mv busybox-$(BUSYBOX_VERSION) busybox-src && \
	  rm $(BUSYBOX_TAR); \
	fi
	@if [ -n "$(BUSYBOX_PATCHES)" ]; then \
	  set -e; \
	  for patch_file in $(BUSYBOX_PATCHES); do \
	    if patch -d "$(BUSYBOX_SRC)" -p1 -N --dry-run < "$$patch_file" >/dev/null 2>&1; then \
	      echo "Applying Busybox patch $$(basename "$$patch_file")"; \
	      patch -d "$(BUSYBOX_SRC)" -p1 -N < "$$patch_file"; \
	    else \
	      echo "Busybox patch $$(basename "$$patch_file") already applied or not applicable"; \
	    fi; \
	  done; \
	fi
	@echo "Configuring Busybox $(BUSYBOX_VERSION) for x86_64-unknown-miniOS + mlibc..."
	mkdir -p $(BUSYBOX_BUILD)
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_BUILD) \
	  CC="clang --target=x86_64-unknown-miniOS" \
	  AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip \
	  CROSS_COMPILE="" allnoconfig
	@set -e; \
	cfg="$(BUSYBOX_BUILD)/.config"; \
	while IFS= read -r line; do \
	  case "$$line" in \
	    ''|'#'*) continue ;; \
	  esac; \
	  key=$${line%%=*}; \
	  if grep -q "^$$key=" "$$cfg"; then \
	    sed -i "s|^$$key=.*|$$line|" "$$cfg"; \
	  elif grep -q "^# $$key is not set$$" "$$cfg"; then \
	    sed -i "s|^# $$key is not set$$|$$line|" "$$cfg"; \
	  else \
	    printf '%s\n' "$$line" >> "$$cfg"; \
	  fi; \
	done < busybox.config
	PATH="$(CURDIR)/build/ld-wrappers:$(PATH)" \
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_BUILD) \
	  CC="clang --target=x86_64-unknown-miniOS" \
	  AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip \
	  CROSS_COMPILE="" \
	  EXTRA_CFLAGS="--sysroot=$(MLIBC_SYSROOT) -isystem $(MLIBC_INCLUDE) -isystem $(BUSYBOX_COMPAT) -D_DEFAULT_SOURCE -D__linux__ -fPIC -Wno-ignored-optimization-argument" \
	  EXTRA_LDFLAGS="-nostdlib -static -fuse-ld=lld \
	    -Wl,-S -Wl,-pie -Wl,$(MLIBC_SYSROOT)/lib/crt1.o \
	    -Wl,-L$(MLIBC_SYSROOT)/lib" \
	  LDLIBS="c miniOS"
	@mkdir -p $(CURDIR)/build/user/out
	cp $(BUSYBOX_BUILD)/busybox $(BUSYBOX_BIN)
	@echo "Busybox installed to $(BUSYBOX_BIN)."

busybox: $(BUSYBOX_BIN)

# ── Busybox (unpatched upstream) ──────────────────────────────────────────────
# Build BusyBox from clean upstream source with no local patches applied.
# Used to validate BB-02: that tracked BusyBox patches can be retired because
# miniOS kernel/libc semantics are sufficient for unmodified upstream applets.
# Swap $(BUSYBOX_UPSTREAM_BIN) into the disk image to run the retirement test.
$(BUSYBOX_UPSTREAM_BIN): busybox.config $(BUSYBOX_COMPAT_LIB) $(LD_WRAPPER) $(MLIBC_LIBC)
	@if [ ! -d "$(BUSYBOX_SRC)" ]; then \
	  echo "Fetching Busybox $(BUSYBOX_VERSION)..." && \
	  wget --show-progress -O $(BUSYBOX_TAR) $(BUSYBOX_URL) && \
	  tar -xjf $(BUSYBOX_TAR) && \
	  mv busybox-$(BUSYBOX_VERSION) busybox-src && \
	  rm $(BUSYBOX_TAR); \
	fi
	@echo "Configuring Busybox $(BUSYBOX_VERSION) (unpatched) for x86_64-unknown-miniOS + mlibc..."
	mkdir -p $(BUSYBOX_UPSTREAM_BUILD)
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_UPSTREAM_BUILD) \
	  CC="clang --target=x86_64-unknown-miniOS" \
	  AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip \
	  CROSS_COMPILE="" allnoconfig
	@set -e; \
	cfg="$(BUSYBOX_UPSTREAM_BUILD)/.config"; \
	while IFS= read -r line; do \
	  case "$$line" in \
	    ''|'#'*) continue ;; \
	  esac; \
	  key=$${line%%=*}; \
	  if grep -q "^$$key=" "$$cfg"; then \
	    sed -i "s|^$$key=.*|$$line|" "$$cfg"; \
	  elif grep -q "^# $$key is not set$$" "$$cfg"; then \
	    sed -i "s|^# $$key is not set$$|$$line|" "$$cfg"; \
	  else \
	    printf '%s\n' "$$line" >> "$$cfg"; \
	  fi; \
	done < busybox.config
	PATH="$(CURDIR)/build/ld-wrappers:$(PATH)" \
	$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_UPSTREAM_BUILD) \
	  CC="clang --target=x86_64-unknown-miniOS" \
	  AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip \
	  CROSS_COMPILE="" \
	  EXTRA_CFLAGS="--sysroot=$(MLIBC_SYSROOT) -isystem $(MLIBC_INCLUDE) -isystem $(BUSYBOX_COMPAT) -D_DEFAULT_SOURCE -D__linux__ -fPIC -Wno-ignored-optimization-argument" \
	  EXTRA_LDFLAGS="-nostdlib -static -fuse-ld=lld \
	    -Wl,-S -Wl,-pie -Wl,$(MLIBC_SYSROOT)/lib/crt1.o \
	    -Wl,-L$(MLIBC_SYSROOT)/lib" \
	  LDLIBS="c miniOS"
	@mkdir -p $(CURDIR)/build/user/out
	cp $(BUSYBOX_UPSTREAM_BUILD)/busybox $(BUSYBOX_UPSTREAM_BIN)
	@echo "Upstream (unpatched) Busybox installed to $(BUSYBOX_UPSTREAM_BIN)."

busybox-upstream: $(BUSYBOX_UPSTREAM_BIN)

# ── lwIP: download + compile ──────────────────────────────────────────────────
$(LWIP_BUILD)/%.o: $(LWIP_SRC)/%.c | $(LWIP_SRC)
	@mkdir -p $(dir $@)
	$(CC_FOR_TARGET) $(LWIP_CFLAGS) -c -o $@ $<

$(LWIP_LIB): $(LWIP_OBJS)
	$(AR_FOR_TARGET) rcs $@ $^
	$(RANLIB_FOR_TARGET) $@

$(LWIP_SRC):
	@echo "Fetching lwIP $(LWIP_VERSION)..."
	wget --show-progress -O $(LWIP_TAR) $(LWIP_URL)
	unzip -q $(LWIP_TAR)
	mv lwip-$(LWIP_VERSION) lwip-src
	rm $(LWIP_TAR)

lwip: $(LWIP_LIB)

# ── Kernel: compile C → .o ────────────────────────────────────────────────────
build/kernel/%.o: src/kernel/%.c | $(LWIP_SRC)
	@mkdir -p $(dir $@)
	$(CC_FOR_TARGET) $(KERNEL_CFLAGS) -MMD -MP -c -o $@ $<

# ── Kernel: archive ───────────────────────────────────────────────────────────
build/kernel/libkernel.a: $(KERNEL_OBJS)
	$(AR_FOR_TARGET) rcs $@ $^
	$(RANLIB_FOR_TARGET) $@

# ── Arch: compile C → .o ──────────────────────────────────────────────────────
GOP_FONT_SRC := /usr/share/kbd/consolefonts/LatArCyrHeb-16.psfu.gz
GOP_FONT_HDR := include/miniOS/drivers/gop_font.h

$(GOP_FONT_HDR): scripts/gen_font.py $(GOP_FONT_SRC)
	python3 $< $(GOP_FONT_SRC) $@

build/arch/%.o: src/arch/x86_64/%.c | $(GOP_FONT_HDR) $(LWIP_SRC)
	@mkdir -p $(dir $@)
	$(CC_FOR_TARGET) $(KERNEL_CFLAGS) -MMD -MP -c -o $@ $<

# ── Arch: compile ASM → .asm.o (disambiguates from same-stem .c objects) ──────
build/arch/%.asm.o: src/arch/x86_64/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

# ── Arch: archive ─────────────────────────────────────────────────────────────
build/arch/libarch.a: $(ARCH_OBJS)
	$(AR_FOR_TARGET) rcs $@ $^
	$(RANLIB_FOR_TARGET) $@

# ── Kernel link ───────────────────────────────────────────────────────────────
dist/x86_64/kernel.bin: build/kernel/libkernel.a build/arch/libarch.a $(LWIP_LIB)
	@mkdir -p dist/x86_64
	$(LD_FOR_TARGET) $(KERNEL_LDFLAGS) \
	  -o $@ \
	  --whole-archive build/kernel/libkernel.a build/arch/libarch.a --no-whole-archive \
	  $(LWIP_LIB)

# ── Userspace sysroot guard ───────────────────────────────────────────────────
$(MLIBC_LIBC):
	$(error mlibc not found at $(MLIBC_LIBC). Run: make mlibc)

# ── Userspace: compile each command's main.c → .o ────────────────────────────
build/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC_USER) $(USER_CFLAGS) -MMD -MP -c -o $@ $<

# ── sh: link ─────────────────────────────────────────────────────────────────
build/user/out/sh: build/user/sh/main.o $(MLIBC_LIBC)
	@mkdir -p $(dir $@)
	$(CC_USER) $(USER_CFLAGS) $(USER_LDFLAGS) -Wl,--build-id=none -o $@ build/user/sh/main.o

# ── Commands: pattern rule ────────────────────────────────────────────────────
$(patsubst %, build/user/out/%, $(USER_CMDS)): build/user/out/%: build/user/%/main.o $(MLIBC_LIBC)
	@mkdir -p $(dir $@)
	$(CC_USER) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ build/user/$*/main.o

# ── testpthread: needs libpthread ─────────────────────────────────────────────
build/user/out/testpthread: build/user/testpthread/main.o $(MLIBC_LIBC)
	@mkdir -p $(dir $@)
	$(CC_USER) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ build/user/testpthread/main.o -lpthread

# ── testptrace: needs libminiOS (ptrace() is a miniOS-specific syscall wrapper) ─
build/user/out/testptrace: build/user/testptrace/main.o $(MLIBC_LIBC) $(BUSYBOX_COMPAT_LIB)
	@mkdir -p $(dir $@)
	$(CC_USER) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ build/user/testptrace/main.o -lminiOS

# ── ISO ───────────────────────────────────────────────────────────────────────
iso: dist/x86_64/kernel.bin
	cp dist/x86_64/kernel.bin targets/x86_64/iso/boot/kernel.bin
	$(STRIP_FOR_TARGET) --strip-all targets/x86_64/iso/boot/kernel.bin
	$(GRUB_MKRESCUE) /usr/lib/grub/i386-pc \
	  -o dist/x86_64/kernel.iso targets/x86_64/iso

# ── testfs-install ────────────────────────────────────────────────────────────
testfs-install: $(USER_BINS) $(BUSYBOX_BIN)
	@# Mount-point stubs — init mounts tmpfs here; dirs must exist on ext2
	@mkdir -p targets/x86_64/testfs/tmp targets/x86_64/testfs/dev
	@# /bin/ — Busybox binary + applet symlinks
	@mkdir -p targets/x86_64/testfs/bin
	@mkdir -p targets/x86_64/testfs/data
	cp $(BUSYBOX_BIN) targets/x86_64/testfs/bin/busybox
	@for applet in $(BUSYBOX_SYMLINKS); do \
	  ln -sf busybox targets/x86_64/testfs/bin/$$applet; \
	done
	@# /bin/ — hand-rolled diagnostic binaries (per D-14, D-15)
	cp --remove-destination build/user/out/ifconfig targets/x86_64/testfs/bin/ifconfig
	cp --remove-destination build/user/out/nc targets/x86_64/testfs/bin/nc
	cp --remove-destination build/user/out/wget targets/x86_64/testfs/bin/wget
	@# /test/bin/ — all hand-rolled binaries (used by regression tests)
	@mkdir -p targets/x86_64/testfs/test/bin
	@for bin in sh hello echo cat testwrite testmmap testmmap_file \
	            ls testmountstat mkdir rm mount testidentity testsignal \
	            testpipe testsyscall27 smptest testtty test_raw_mode test_winsize \
	            test_sigint print_env test_env \
	            lspci udp_echo tcp_connect rawtest nc wget \
	            testunix testfutex testjobctrl testptrace testpthread; do \
	  cp build/user/out/$$bin targets/x86_64/testfs/test/bin/$$bin; \
	done
	cp build/user/out/init targets/x86_64/testfs/init

# ── Disk image ────────────────────────────────────────────────────────────────
# Partition layout (512-byte sectors):
#   p1: LBA   2048 – 133119  (131072 sectors =  64 MiB, ext2, Linux filesystem)
#   p2: LBA 135168 – 397311  (262144 sectors = 128 MiB, FAT32, Microsoft basic data)
#   Total disk: 200 MiB (409600 sectors)
disk-img: iso testfs-install
	@mkdir -p dist/x86_64
	$(MKE2FS) -q -b 1024 -t ext2 -F \
	  -d targets/x86_64/testfs \
	  dist/x86_64/disk.part 65536
	dd if=/dev/zero bs=1M count=128 of=dist/x86_64/fat32.part status=none
	$(MKFSFAT) -F 32 -n "DATA" dist/x86_64/fat32.part
	@find targets/x86_64/data -mindepth 1 -maxdepth 1 -not -name '.gitkeep' \
	    -exec $(MCOPY) -i dist/x86_64/fat32.part -s {} :: \;
	dd if=/dev/zero bs=1M count=200 of=dist/x86_64/disk.img status=none
	dd if=dist/x86_64/disk.part of=dist/x86_64/disk.img \
	  bs=512 seek=2048 conv=notrunc status=none
	dd if=dist/x86_64/fat32.part of=dist/x86_64/disk.img \
	  bs=512 seek=135168 conv=notrunc status=none
	sh scripts/create-gpt-disk.sh dist/x86_64/disk.img 2048 131072 135168 262144
	rm -f dist/x86_64/disk.part dist/x86_64/fat32.part

# ── QEMU interactive (no isa-debug-exit) ─────────────────────────────────────
# SLIRP (libslirp ≥ QEMU 5) removed the old built-in echo service.
# It still proxies 10.0.2.2:PORT → 127.0.0.1:PORT on the host, so a local
# socat echo server satisfies udp_echo without any QEMU flag changes.
UDP_ECHO_PID_FILE := /tmp/miniOS-udp-echo.pid
TCP_ECHO_PID_FILE := /tmp/miniOS-tcp-echo.pid

udp-echo-start:
	@if [ -f $(UDP_ECHO_PID_FILE) ] && kill -0 $$(cat $(UDP_ECHO_PID_FILE)) 2>/dev/null; then \
	  echo "udp-echo: already running (pid $$(cat $(UDP_ECHO_PID_FILE)))"; \
	else \
	  socat UDP4-RECVFROM:10007,fork EXEC:'cat' & \
	  echo $$! > $(UDP_ECHO_PID_FILE); \
	  echo "udp-echo: started socat echo server on :10007 (pid $$!)"; \
	fi

udp-echo-stop:
	@if [ -f $(UDP_ECHO_PID_FILE) ]; then \
	  kill $$(cat $(UDP_ECHO_PID_FILE)) 2>/dev/null || true; \
	  rm -f $(UDP_ECHO_PID_FILE); \
	  echo "udp-echo: stopped"; \
	fi

tcp-echo-start:
	@if [ -f $(TCP_ECHO_PID_FILE) ] && kill -0 $$(cat $(TCP_ECHO_PID_FILE)) 2>/dev/null; then \
	  echo "tcp-echo: already running (pid $$(cat $(TCP_ECHO_PID_FILE)))"; \
	else \
	  socat TCP4-LISTEN:10007,fork,reuseaddr EXEC:'cat' & \
	  echo $$! > $(TCP_ECHO_PID_FILE); \
	  echo "tcp-echo: started socat echo server on :10007 (pid $$!)"; \
	fi

tcp-echo-stop:
	@if [ -f $(TCP_ECHO_PID_FILE) ]; then \
	  kill $$(cat $(TCP_ECHO_PID_FILE)) 2>/dev/null || true; \
	  rm -f $(TCP_ECHO_PID_FILE); \
	  echo "tcp-echo: stopped"; \
	fi

qemu: qemu-smp4

NET_DUMP     ?= 0
NET_DUMP_FILE ?= /tmp/miniOS-net.pcap
# NET_DUMP=1 splits -nic into -netdev/-device so filter-dump can attach.
# Open the resulting file with:  wireshark /tmp/miniOS-net.pcap
ifeq ($(NET_DUMP),1)
QEMU_NET_ARGS = \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0,disable-modern=on \
  -object filter-dump,id=fd0,netdev=net0,file=$(NET_DUMP_FILE)
else
QEMU_NET_ARGS = -nic user,model=virtio-net-pci
endif

qemu-smp1: iso disk-img udp-echo-start tcp-echo-start
	$(QEMU) -m 64 -no-reboot -no-shutdown -smp 1 -cpu Haswell \
	  -boot order=d \
	  -cdrom dist/x86_64/kernel.iso \
	  $(QEMU_NET_ARGS) \
	  -drive file=dist/x86_64/disk.img,format=raw,if=ide,index=0

qemu-smp4: iso disk-img udp-echo-start tcp-echo-start
	$(QEMU) -m 64 -no-reboot -no-shutdown -smp 4 -cpu Haswell \
	  -boot order=d \
	  -cdrom dist/x86_64/kernel.iso \
	  $(QEMU_NET_ARGS) \
	  -drive file=dist/x86_64/disk.img,format=raw,if=ide,index=0

qemu-debug: iso disk-img
	$(QEMU) -m 16 -d int,cpu_reset -no-reboot -no-shutdown -cpu Haswell \
	  -boot order=d \
	  -cdrom dist/x86_64/kernel.iso \
	  -nic user,model=virtio-net-pci,rombar=0 \
	  -drive file=dist/x86_64/disk.img,format=raw,if=ide,index=0 \
	  -monitor stdio

# ── Host unit tests ───────────────────────────────────────────────────────────
check:
	$(MAKE) -C tests run

test: check check-functional test-terminal test-env

# ── Functional tests ──────────────────────────────────────────────────────────
check-functional: iso disk-img
	bash tests/functional/run.sh

test-terminal: iso disk-img
	bash tests/functional/test_terminal.sh

test-signal-parity: iso disk-img testfs-install
	bash tests/functional/test_signal_timer_parity.sh

test-env: iso disk-img
	bash tests/functional/test_env.sh

test-network-smoke: iso
	bash tests/functional/test_network_smoke.sh

test-udp-echo: disk-img
	bash tests/functional/test_udp_echo.sh

test-tcp-connect: disk-img tcp-echo-start
	bash tests/functional/test_tcp_connect.sh

# test-raw-socket requires tshark; kept as standalone target (not in test-functional per D-18)
test-raw-socket: disk-img
	bash tests/functional/test_raw_socket.sh

test-ping: disk-img
	bash tests/functional/test_ping.sh

test-nc: disk-img
	bash tests/functional/test_nc.sh

test-wget: disk-img
	bash tests/functional/test_wget.sh

test-busybox-compat: iso disk-img testfs-install
	bash tests/functional/test_busybox_compat.sh

test-busybox-upstream: iso busybox-upstream
	bash tests/functional/test_busybox_upstream.sh

test-functional: check-functional test-terminal test-env test-udp-echo test-tcp-connect test-ping test-nc test-wget

# ── Doxygen ───────────────────────────────────────────────────────────────────
docs:
	@if command -v doxygen >/dev/null 2>&1; then \
	  doxygen docs/Doxyfile; \
	  scripts/gen-sitemap.sh; \
	else \
	  echo "WARNING: doxygen not found; 'make docs' is a no-op"; \
	fi

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf build/ dist/ _busybox-build/ _busybox-upstream-build/ _lwip-build/ _mlibc-build/
	rm -f $(BUSYBOX_COMPAT_LIB)

clean-mlibc:
	rm -rf _mlibc-build/ _mlibc-src/ $(MLIBC_SYSROOT)/

# ── Auto-generated dependency files ───────────────────────────────────────────
-include $(shell find build -name '*.d' 2>/dev/null)
