# tests/Makefile — hand-written host-native Unity test build
# NOT wrapped in autotools — top-level Makefile.am calls $(MAKE) -C tests

CC = gcc

# lwip-src lives at the main repo root (not the worktree root).
# Use --git-common-dir to locate the shared .git, then strip the .git suffix.
MAIN_REPO := $(shell git -C $(CURDIR) rev-parse --git-common-dir 2>/dev/null | xargs dirname)
LWIP_INC  := $(MAIN_REPO)/lwip-src/src/include

CFLAGS = -Wall -Wextra -g -O0 --std=c2x -DTEST_BUILD \
         -I stubs \
         -I vendor \
         -I $(LWIP_INC) \
         -I ../include/miniOS/net \
         -idirafter ../include \
         -include stubs/stub_types.h \
         -include stubs/stub_printk.h

UNITY = vendor/unity.c

TESTS = test_pmm test_heap test_vfs test_syscall test_elf test_keyboard \
        test_mount_table test_mount_routing test_tmpfs test_pipe \
        test_spinlock test_sched_percpu test_sched_loadbalance \
        test_sched_worksteal test_sched_ipi test_ipi_barrier test_ipi_panic_halt \
        test_vsnprintf test_console test_net_ethernet test_rtl8139 \
        test_lwip_port test_net_config test_net_observe test_net_socket \
        test_fat32

.PHONY: all run clean

all: $(TESTS)

test_pmm: unit/test_pmm.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_heap: unit/test_heap.c stubs/stub_pmm.c stubs/stub_vmm.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_vfs: unit/test_vfs.c stubs/stub_sched.c stubs/stub_keyboard.c stubs/stub_pmm.c stubs/stub_vmm.c stubs/stub_pipe.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_syscall: unit/test_syscall.c stubs/stub_sched.c stubs/stub_keyboard.c stubs/stub_vfs.c stubs/stub_pmm.c stubs/stub_vmm.c stubs/stub_elf.c stubs/stub_heap.c stubs/stub_lwip.c stubs/stub_net_socket.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_elf: unit/test_elf.c stubs/stub_sched.c stubs/stub_keyboard.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_keyboard: unit/test_keyboard.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_mount_table: unit/test_mount_table.c stubs/stub_sched.c stubs/stub_keyboard.c stubs/stub_pmm.c stubs/stub_vmm.c stubs/stub_pipe.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_mount_routing: unit/test_mount_routing.c stubs/stub_sched.c stubs/stub_keyboard.c stubs/stub_pmm.c stubs/stub_vmm.c stubs/stub_pipe.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_tmpfs: unit/test_tmpfs.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_pipe: unit/test_pipe.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_spinlock: unit/test_spinlock.c ../src/arch/x86_64/smp/spinlock.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_sched_percpu: unit/test_sched_percpu.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_sched_loadbalance: unit/test_sched_loadbalance.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_sched_worksteal: unit/test_sched_worksteal.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_sched_ipi: unit/test_sched_ipi.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_ipi_barrier: unit/test_ipi_barrier.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_ipi_panic_halt: unit/test_ipi_panic_halt.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_vsnprintf: unit/test_vsnprintf.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_console: unit/test_console.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_net_ethernet: unit/test_net_ethernet.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_rtl8139: unit/test_rtl8139.c stubs/stub_pci.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ $^

test_lwip_port: unit/test_lwip_port.c ../src/kernel/net/netdev.c ../src/kernel/net/lwip_port.c stubs/stub_sched.c stubs/stub_lwip.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ unit/test_lwip_port.c stubs/stub_sched.c stubs/stub_lwip.c $(UNITY)

test_net_config: unit/test_net_config.c ../src/kernel/net/netdev.c ../src/kernel/net/net_config.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ unit/test_net_config.c $(UNITY)

test_net_observe: unit/test_net_observe.c ../src/kernel/fs/sysfs.c ../src/kernel/net/netdev.c ../src/kernel/net/net_config.c stubs/stub_sched.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ unit/test_net_observe.c stubs/stub_sched.c $(UNITY)

test_net_socket: unit/test_net_socket.c stubs/stub_lwip.c $(UNITY)
	$(CC) $(CFLAGS) -o $@ unit/test_net_socket.c stubs/stub_lwip.c $(UNITY)

test_fat32: unit/test_fat32.c
	$(CC) $(CFLAGS) -o $@ unit/test_fat32.c

run: all
	@PASS=0; FAIL=0; \
	for t in $(TESTS); do \
	  printf "  %-35s " $$t; \
	  if ./$$t > /dev/null 2>&1; then \
	    echo "PASS"; PASS=$$((PASS+1)); \
	  else \
	    echo "FAIL"; FAIL=$$((FAIL+1)); \
	  fi; \
	done; \
	echo ""; \
	echo "Results: $$PASS passed, $$FAIL failed"; \
	[ $$FAIL -eq 0 ]

clean:
	rm -f $(TESTS)
