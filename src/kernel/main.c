// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <miniOS/arch/x86_64/gdt.h>
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/exceptions.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/drivers/console.h>
#include <miniOS/drivers/keyboard.h>
#include <miniOS/io.h>
#include <string.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/mm/heap.h>
#include <miniOS/sched/sched.h>
#include <miniOS/syscall.h>
#include <miniOS/userspace/enter_ring3.h>
#include <miniOS/drivers/ata.h>
#include <miniOS/drivers/pci.h>
#include <miniOS/fs/ext2.h>
#include <miniOS/fs/fat32.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/fs/tmpfs.h>
#include <miniOS/fs/sysfs.h>
#include <miniOS/fs/procfs.h>
#include <miniOS/fs/elf.h>
#include <miniOS/arch/x86_64/tss.h>
#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/qemu_exit.h>
#include <miniOS/drivers/block_layer.h>
#include <miniOS/drivers/gpt.h>
#include <miniOS/drivers/block_device.h>
#include <miniOS/net/netdev.h>
#include <miniOS/net/net_observe.h>
#include <miniOS/net/lwip_netif.h>
#ifdef CONSOLE_GOP
#include <miniOS/drivers/vt.h>
#endif

#ifdef CONSOLE_GOP
struct __attribute__((packed)) mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    uint8_t  red_pos;
    uint8_t  red_size;
    uint8_t  green_pos;
    uint8_t  green_size;
    uint8_t  blue_pos;
    uint8_t  blue_size;
};

static void gop_init_from_mb2(uint64_t mb_info_phys) {
    const uint8_t *mb2 = (const uint8_t *)(mb_info_phys + KERNEL_VMA);
    uint32_t total = *(const uint32_t *)mb2;
    const uint8_t *p = mb2 + 8;   /* skip total_size + reserved */

    while (p < mb2 + total) {
        uint32_t tag_type = *(const uint32_t *)p;
        uint32_t tag_size = *(const uint32_t *)(p + 4);
        if (tag_type == 0) break;

        if (tag_type == 8) {
            const struct mb2_tag_framebuffer *fb =
                (const struct mb2_tag_framebuffer *)p;
            if (fb->fb_type == 1 && fb->bpp == 32) {
                uint64_t phys = fb->addr;
                uint32_t sz   = fb->height * fb->pitch;
                for (uint32_t off = 0; off < sz; off += 4096)
                    vmm_map_page(GOP_FB_VA + off, phys + off,
                                 PAGE_PRESENT | PAGE_WRITE);
                vt_init_gop(GOP_FB_VA, fb->width, fb->height, fb->pitch,
                            fb->red_pos, fb->green_pos, fb->blue_pos);
                printk("GOP: %ux%u bpp=%u pitch=%u phys=0x%llx\n",
                       fb->width, fb->height, fb->bpp, fb->pitch, phys);
                return;
            }
        }

        p += (tag_size + 7u) & ~7u;
    }
    printk("GOP: no 32bpp framebuffer tag — staying on VGA text mode\n");
}
#endif

/* Demo threads removed: userspace ring-3 demo now runs instead. */
/* bsp_idle_resume: BSP idle loop entered after user task exits via SYS_exit.
 * Must be a standalone function — using &&label inside kernel_main is
 * unreliable because GCC may place the label in the middle of setup code,
 * causing context_switch_asm to jump to instructions that expect non-zero
 * registers from the setup fall-through path.
 * Named distinctly from sched.c's idle_loop (which is now non-static). */
static __attribute__((noreturn)) void bsp_idle_resume(void)
{
    /* The LAPIC timer can preempt a running user task and schedule the idle
     * thread before the user task has actually exited.  Spin here until all
     * non-idle threads are dead, then signal completion to QEMU. */
    while (sched_has_user_threads())
        __asm__ volatile("sti; hlt" ::: "memory");

    printk("Userspace: idle resumed after user task exit\n");
    /* Signal functional test completion to QEMU isa-debug-exit device.
     * qemu_exit(0) causes QEMU to exit with code 1 (PASS signal).
     * Interactive QEMU targets do not include the device; the outb is a no-op. */
    qemu_exit(0);
    for (;;) __asm__ volatile("sti; hlt");
}

void cpu_enable_sse(void)
{
    uint64_t cr0;
    uint64_t cr4;
    static const uint32_t default_mxcsr = 0x1F80;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); /* clear EM */
    cr0 |= (1ULL << 1);  /* set MP */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    __asm__ volatile("clts");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  /* OSFXSR */
    cr4 |= (1ULL << 10); /* OSXMMEXCPT */

    /* FSGSBASE/SMEP/SMAP all require CPUID leaf 7, EBX support. */
    {
        uint32_t leaf7_eax = 7, leaf7_ecx = 0, leaf7_ebx = 0, leaf7_edx = 0;
        __asm__ volatile("cpuid"
            : "=a"(leaf7_eax), "=b"(leaf7_ebx), "=c"(leaf7_ecx), "=d"(leaf7_edx)
            : "0"(leaf7_eax), "2"(leaf7_ecx));
        if (leaf7_ebx & (1U << 0))
            cr4 |= (1ULL << 16); /* FSGSBASE */
        if (leaf7_ebx & (1U << 7))
            cr4 |= (1ULL << 20); /* SMEP */
        if (leaf7_ebx & (1U << 20))
            cr4 |= (1ULL << 21); /* SMAP */
    }

    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" :: "m"(default_mxcsr));
}


void kernel_main(uint64_t mb_info_phys) {
    /* Clear debug registers: BIOS or QEMU may leave stale hardware breakpoints
     * in DR0-DR7.  DR7=0 disables all hardware breakpoints; DR6=0 clears the
     * debug status so a stale BS (single-step) or Bx bit does not trigger a
     * spurious #DB when we first enable interrupts. */
    __asm__ volatile(
        "xor %%eax, %%eax\n\t"
        "mov %%rax, %%dr6\n\t"
        "mov %%rax, %%dr7\n\t"
        ::: "rax"
    );

    console_init();
    printk("Welcome to miniOS\n");

    pmm_init(mb_info_phys);
    printk("PMM: %llu free frames (%llu MB)\n",
           pmm_free_count(), pmm_free_count() * 4 / 1024);

    printk("Heap: init...\n");
    heap_init();
    printk("Heap: free bytes = %llu\n", heap_free_bytes());

#ifdef CONSOLE_GOP
    gop_init_from_mb2(mb_info_phys);
#endif

    /* Initialize scheduler early so sched_current() is valid before VFS calls */
    sched_init();

    sysfs_init();
    procfs_init();
    pci_init();
    net_init();
    net_observe_init();
    lwip_netif_init();
    /* Brief RX flush: drain frames buffered in NIC before userspace starts.
     * Gives the smoke-test injector time to queue ARP/IPv4 frames via the
     * socket NIC backend while the rest of early-boot init runs. */
    for (int _net_i = 0; _net_i < 2000; _net_i++) netdev_poll_all();

    /* Register tmpfs filesystem type and pre-mount /tmp + /dev so they are
     * always available before init runs. */
    tmpfs_init();
    vfs_mount_fstype("tmpfs",  NULL, "/tmp",  NULL);
    vfs_mount_fstype("tmpfs",  NULL, "/dev",  NULL);
    vfs_mount_fstype("procfs", NULL, "/proc", NULL);
    vfs_mknod("/dev/null", (uint16_t)(EXT2_S_IFCHR | 0666), (1U << 8) | 3U);
    vfs_mknod("/dev/tty",  (uint16_t)(EXT2_S_IFCHR | 0666), (5U << 8) | 0U);
    printk("VFS: /dev/null and /dev/tty created\n");

    /* Register ext2 filesystem type so vfs_mount_fstype("ext2", ...) works */
    ext2_init();
    /* Register FAT32 filesystem type so vfs_mount_fstype("fat32", ...) works */
    fat32_init();

    /* Storage smoke test ---------------------------------------- */
    printk("Storage: init ATA driver...\n");
    if (ata_init() != 0) {
        printk("Storage: ATA init FAILED\n");
    } else {
        printk("Storage: ATA init OK\n");

        block_layer_init();
        blkdev_init();

        /* Register whole disk as /dev/sda (major 8, minor 0, Linux convention). */
        blkdev_register("sda", BLKDEV_MAJOR_SATA, 0, 0, 0, false);
        vfs_mknod("/dev/sda", (uint16_t)(EXT2_S_IFBLK | 0660), (8U << 8) | 0U);

        /* Parse GPT partition table and register each partition. */
        gpt_partition_t parts[8];
        int nparts = 0;
        if (gpt_parse(parts, 8, &nparts) == 0 && nparts > 0) {
            char devname[16];
            char devpath[24];
            for (int i = 0; i < nparts; i++) {
                snprintf(devname, sizeof(devname), "sda%d", i + 1);
                blkdev_register(devname, BLKDEV_MAJOR_SATA, (uint8_t)(i + 1),
                                parts[i].lba_start, parts[i].lba_end, true);
                snprintf(devpath, sizeof(devpath), "/dev/%s", devname);
                vfs_mknod(devpath, (uint16_t)(EXT2_S_IFBLK | 0660),
                          (8U << 8) | (uint32_t)(i + 1));
            }

            /* Mount ext2 from the first GPT partition. */
            vfs_mount_data_t ext2_data = {
                .major = BLKDEV_MAJOR_SATA,
                .minor = 1,
            };
            printk("Storage: mounting ext2 at /dev/sda1...\n");
            if (vfs_mount_fstype("ext2", NULL, "/", &ext2_data) != 0)
                printk("Storage: ext2 mount FAILED\n");
        } else {
            printk("Storage: GPT parse failed or no partitions found\n");
        }
    }

    printk("Initialize GDT and IDT... ");
    gdt_init();
    idt_init();
    exceptions_init();
    cpu_enable_sse();
    printk("done\n");

    syscall_init();

    printk("Initialize LAPIC (disable legacy PIC)... ");
    lapic_init();
    printk("done\n");

    printk("Initialize I/O APIC... ");
    ioapic_init();
    printk("done\n");

    /* Keyboard init — must be after ioapic_init() which masks all IRQs.
     * ioapic_init() resets all IOAPIC RTE entries to masked; calling keyboard_init()
     * here ensures IRQ1 stays unmasked when the shell runs. */
    keyboard_init();
    printk("Keyboard: driver ready\n");

    __asm__ volatile("sti");
    printk("Interrupts enabled\n");

    per_cpu_init(0);  /* Initialize BSP per-CPU state: GDT copy, TSS, GS-base MSR */
    printk("BSP: cpu_local()->cpu_id=%u\n", cpu_local()->cpu_id);

    printk("SMP: scanning ACPI MADT...\n");
    acpi_find_madt(mb_info_phys);

    /* Install IDT handler for LAPIC timer vector BEFORE booting APs.
       APs start their LAPIC timers in lapic_timer_init_ap(); without a valid
       IDT[LAPIC_TIMER_VECTOR] handler they would #GP → ipi_panic_halt → halt BSP. */
    lapic_timer_idt_install();

    printk("SMP: booting %u APs...\n", smp_cpu_count > 0 ? smp_cpu_count - 1 : 0);
    smp_boot_aps();

    printk("Initialize LAPIC timer... ");
    lapic_timer_init();
    printk("done\n");

    /* SMP barrier: BSP blocks until all APs have completed per_cpu_init(),
     * started their LAPIC timers via lapic_timer_init_ap(), and called
     * ap_barrier_checkin(). Only then does the BSP continue to scheduler init.
     * This ensures all CPUs are online with per-CPU state before sched_init(). */
    smp_wait_for_aps();

    /* Scheduler — already initialized before storage test; re-init
     * here resets the idle thread cleanly before user-task creation. */
    sched_init();

    /* Create per-CPU idle threads for all APs.
     * APs are currently halting in ap_entry() after checking in; they will
     * pick up their idle threads when the LAPIC timer fires and sched_tick()
     * selects the next runnable thread from the shared run queue. */
    for (uint32_t i = 1; i < smp_cpu_count; i++) {
        sched_init_cpu(i);
    }

    /* Remove the boot identity map (PML4[0]) so that vmm_map_page can create
     * fresh user-space page tables at 0x400000.  The boot code set up PML4[0]
     * with 2 MB huge-page entries covering 0..1 GB; those entries would
     * confuse vmm_map_page's get_or_create_entry walk.  The kernel has been
     * running exclusively in the higher-half (PML4[256]) since main64.asm, so
     * clearing PML4[0] is safe.
     */
    {
        uint64_t cr3_phys;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_phys));
        /* PML4 is at cr3_phys + KERNEL_VMA (lower 1 GB is mapped there). */
        uint64_t *pml4 = (uint64_t *)(cr3_phys + KERNEL_VMA);
        pml4[0] = 0;  /* clear identity map entry */
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3_phys) : "memory"); /* full TLB flush */
    }

    /* Launch init process from disk ------------------------------------ */
    printk("Init: opening /init...\n");
    int init_fd = vfs_open("/init", 0 /* O_RDONLY */, 0);
    
    uint64_t init_entry = 0;
    uint64_t init_image_end = 0;
    uint64_t init_phdr_va = 0;
    uint16_t init_phnum = 0;
    struct thread *init_t = NULL;

    if (init_fd >= 0) {
        /* Create the task (and its own address space, per-process PML4) BEFORE
         * elf_load() — elf_load's vmm_map_page() calls target whatever CR3 is
         * currently live, so CR3 must already be init's own table by the time
         * it runs, not the boot PML4 sched_create_user_task allocated init's
         * table separately from. */
        init_t = sched_create_user_task(0);
        if (init_t) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(init_t->pml4_phys) : "memory");

            if (elf_load(init_fd, &init_entry, &init_image_end, &init_phdr_va, &init_phnum) == 0 && init_entry != 0) {
                printk("Init: ELF loaded, entry=0x%llx phdr_va=0x%llx phnum=%u\n",
                       init_entry, init_phdr_va, (uint32_t)init_phnum);

                init_t->ctx.rip = init_entry;
                /* Preserve the boot image end so fork/wait can snapshot and restore
                 * init's code/data pages just like execve-loaded tasks. */
                init_t->brk = init_image_end;
                init_t->brk_base = init_image_end;
                printk("Init: user task created (tid=%u), entering ring 3...\n", init_t->tid);
                // Init will set up its own environment, so don't pass envp
                init_t->ctx.r15 = 0; /* No envp for init */
            } else {
                init_t = NULL;  /* elf_load failed — fall back to /bin/sh below */
            }
        }
        vfs_close(init_fd);
    }
    
    // Fallback to /bin/sh if /init failed
    if (!init_t) {
        printk("Init: FAIL cannot load /init, falling back to /bin/sh\n");
        int shell_fd = vfs_open("/bin/sh", 0 /* O_RDONLY */, 0);
        if (shell_fd < 0) {
            printk("Shell: FAIL cannot open /bin/sh\n");
            for (;;) __asm__ volatile("hlt");
        }

        init_t = sched_create_user_task(0);
        if (!init_t) {
            printk("Shell: FAIL create user task\n");
            for (;;) __asm__ volatile("hlt");
        }
        __asm__ volatile("mov %0, %%cr3" :: "r"(init_t->pml4_phys) : "memory");

        uint64_t shell_entry = 0;
        uint64_t shell_image_end = 0;
        if (elf_load(shell_fd, &shell_entry, &shell_image_end, &init_phdr_va, &init_phnum) != 0 || shell_entry == 0) {
            printk("Shell: FAIL elf_load\n");
            vfs_close(shell_fd);
            for (;;) __asm__ volatile("hlt");
        }
        vfs_close(shell_fd);

        init_t->ctx.rip = shell_entry;
        init_t->brk = shell_image_end;
        init_t->brk_base = shell_image_end;

        char *envp[] = { "TERM=xterm-256color", NULL };
        uint64_t envp_user_ptr = 0;
        if (copy_string_array_to_user(init_t, envp, &envp_user_ptr) != 0) {
            printk("Shell: FAIL copy envp\n");
            for (;;) __asm__ volatile("hlt");
        }
        init_t->ctx.r15 = envp_user_ptr;
        init_entry = shell_entry;   /* fix: enter_ring3 uses init_entry */
    }


    /* Save idle context and hand off to shell */
    struct thread *idle = sched_current();
    uint64_t idle_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(idle_rsp));
    idle->ctx.rip    = (uint64_t)bsp_idle_resume;
    idle->ctx.rsp    = idle_rsp;
    idle->ctx.rflags = 0x202;
    idle->ctx.cs     = 0x08;
    idle->ctx.ss     = 0x10;

    /* Prime this CPU's per-CPU kernel stack pointer BEFORE entering ring 3.
     * per_cpu_update_rsp0() sets both percpu_tss[cpu_id].privileged_stack_table[0]
     * (TSS RSP0, used by hardware interrupts from ring 3) and cpu->kstack_top
     * (gs:64, used by syscall_entry on SYSCALL from ring 3).  sched_schedule()
     * updates these on every subsequent context switch; this call primes them
     * for the very first ring-3 entry before the scheduler has run once. */
    uint64_t init_kstack_top = (uint64_t)(init_t->stack_base + THREAD_STACK_SIZE);
    per_cpu_update_rsp0(init_kstack_top);

    /* Build ELF initial stack: argc/argv/envp/auxv for mlibc's interpreterMain.
     * mlibc needs AT_PHDR+AT_PHNUM to scan program headers (TLS, etc.) and
     * AT_ENTRY to know the real entry point for static-pie startup. */
    uint64_t new_rsp = init_t->user_stack_virt_top - 192; /* 16-byte aligned */
    uint64_t *sp = (uint64_t *)new_rsp;
    sp[0]  = 0;              /* argc */
    sp[1]  = 0;              /* argv[0] = NULL */
    sp[2]  = 0;              /* envp[0] = NULL */
    sp[3]  = 3;              /* AT_PHDR */
    sp[4]  = init_phdr_va;
    sp[5]  = 5;              /* AT_PHNUM */
    sp[6]  = init_phnum;
    sp[7]  = 6;              /* AT_PAGESZ */
    sp[8]  = 4096;
    sp[9]  = 9;              /* AT_ENTRY */
    sp[10] = init_entry;
    sp[11] = 0;              /* AT_NULL */
    sp[12] = 0;

    printk("Init: stack rsp=0x%llx argc=%llu\n", new_rsp, sp[0]);
    printk("Init: auxv AT_PHDR(3)=0x%llx AT_PHNUM(5)=%llu AT_PAGESZ(6)=%llu AT_ENTRY(9)=0x%llx\n",
           sp[4], sp[6], sp[8], sp[10]);
    /* Verify ELF header bytes are still intact at base before entering ring3 */
    {
        uint64_t *hdr = (uint64_t *)ELF_PIE_BASE;
        printk("Init: ELF@base magic=%016llx e_entry=%016llx\n", hdr[0], hdr[3]);
    }

    sched_activate_task(init_t);

    /* Dedicated kernel thread to drive lwIP polling so userspace threads can
     * sleep in poll/select and be woken by recv callbacks. Created after
     * sched_activate_task so the scheduler is fully live before this thread
     * becomes runnable. */
    struct thread *net_poll_t = sched_create_thread(net_poll_thread_fn, NULL);
    if (net_poll_t)
        net_poll_t->state = THREAD_RUNNABLE;

    enter_ring3(init_entry, new_rsp);
}
