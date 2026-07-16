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

#include <miniOS/syscall.h>
#include <miniOS/sched/sched.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/fs/elf.h>
#include <miniOS/mm/heap.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/io.h>
#include <miniOS/signal.h>
#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/net/lwip_netif.h>
#include <miniOS/types.h>
#include <string.h>
#include <stdint.h>
#include "syscall_internal.h"

extern uint64_t miniOS_tsc_hz;
static inline uint64_t proc_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#ifndef MINI_OS_DEBUG_SYSCALL_PROC
#define MINI_OS_DEBUG_SYSCALL_PROC 0
#endif

#if MINI_OS_DEBUG_SYSCALL_PROC
#define SYSCALL_PROC_DEBUG_PRINT(...) printk(__VA_ARGS__)
#else
#define SYSCALL_PROC_DEBUG_PRINT(...) do { } while (0)
#endif

/* wait4/waitpid status-word encoding, matching the Linux ABI mlibc's
 * <sys/wait.h> WIFEXITED/WIFSIGNALED/WIFSTOPPED/WEXITSTATUS/WTERMSIG/WSTOPSIG
 * macros expect. signal.c's default-terminate path stores exit_code = 128 +
 * signum for a
 * thread killed by an uncaught signal (128+1..128+31); anything else is a
 * real exit(2) status. */
static inline int wait_status_encode_exit(int exit_code)
{
    if (exit_code >= 128 && exit_code <= 128 + 31)
        return exit_code - 128;             /* WIFSIGNALED: low byte = termsig */
    return (exit_code & 0xff) << 8;         /* WIFEXITED: exit status in bits 8-15 */
}

static inline int wait_status_encode_stop(int stop_signal)
{
    return 0x7f | ((stop_signal & 0xff) << 8);  /* WIFSTOPPED sentinel + WSTOPSIG */
}

/* ptrace(2) request numbers — real Linux x86-64 values, so a real
 * <sys/ptrace.h> definition would line up if one were ever linked in place
 * of busybox-compat/sys/ptrace.h. Scope: PTRACE_TRACEME only (no
 * PTRACE_ATTACH to an already-running unrelated process). */
#define PTRACE_TRACEME    0
#define PTRACE_PEEKTEXT   1
#define PTRACE_PEEKDATA   2
#define PTRACE_POKETEXT   4
#define PTRACE_POKEDATA   5
#define PTRACE_CONT       7
#define PTRACE_KILL       8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS    12
#define PTRACE_SETREGS    13
#define PTRACE_SYSCALL    24

/* Linux x86-64 struct user_regs_struct layout (27 uint64_t fields) — only
 * the fields miniOS can reliably populate are filled; the rest are zeroed.
 * rip/rsp/rbp/rbx/r12-r15/eflags come from struct thread's saved_user_*
 * fields (always valid while stopped). orig_rax/rdi/rsi/rdx/r10/r8/r9 come
 * from syscall_restart_nr/args (captured at every syscall entry) and are
 * only meaningful at a PTRACE_SYSCALL stop. rax is ptrace_syscall_ret at a
 * syscall-exit stop, 0 otherwise (not yet known). cs/ss are the fixed
 * selector constants; ds/es/fs/gs/fs_base/gs_base are not tracked and read
 * as 0. */
struct miniOS_user_regs_struct {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    uint64_t rax, rcx, rdx, rsi, rdi, orig_rax;
    uint64_t rip, cs, eflags, rsp, ss;
    uint64_t fs_base, gs_base, ds, es, fs, gs;
};

/* Copy len bytes from a (possibly not currently loaded) tracee address
 * space into a kernel buffer, one page at a time via the KERNEL_VMA
 * direct-map alias. Returns 0 on success, -1 if any covered page isn't
 * mapped. */
static int ptrace_copy_from_tracee(struct thread *target, uint64_t va, void *kbuf, size_t len)
{
    uint8_t *dst = (uint8_t *)kbuf;
    uint64_t pml4 = THREAD_PML4(target);
    while (len > 0) {
        uint64_t page_va = va & ~(PAGE_SIZE - 1);
        uint64_t off = va & (PAGE_SIZE - 1);
        uint64_t phys = vmm_virt_to_phys_in(pml4, page_va);
        if (!phys) return -1;
        size_t chunk = (size_t)(PAGE_SIZE - off);
        if (chunk > len) chunk = len;
        memcpy(dst, (void *)(phys + KERNEL_VMA + off), chunk);
        dst += chunk; va += chunk; len -= chunk;
    }
    return 0;
}

/* If the page at @page_va is read-only or CoW-shared, give it a private
 * writable copy first (mirrors vmm_resolve_user_fault's CoW-copy path, but
 * triggered by PTRACE_POKE* instead of a page fault). No-op if already
 * private+writable. Returns 0 on success, -1 on OOM or unmapped page. */
static int ptrace_ensure_writable(struct thread *target, uint64_t page_va)
{
    uint64_t pml4 = THREAD_PML4(target);
    uint64_t pte = vmm_virt_to_pte_in(pml4, page_va);
    if (!(pte & PAGE_PRESENT)) return -1;
    if ((pte & PAGE_WRITE) && !(pte & PAGE_COW)) return 0;

    uint64_t old_phys = pte & PTE_ADDR_MASK;
    uint64_t new_phys = pmm_alloc_frame();
    if (!new_phys) return -1;
    memcpy((void *)(new_phys + KERNEL_VMA), (void *)(old_phys + KERNEL_VMA), PAGE_SIZE);
    if (vmm_map_page_in(pml4, page_va, new_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) < 0) {
        pmm_free_frame(new_phys);
        return -1;
    }
    pmm_unref_frame(old_phys);
    return 0;
}

/* Copy len bytes from a kernel buffer into the tracee's address space,
 * making each touched page privately writable first. Returns 0 on success,
 * -1 if any covered page isn't mapped or a private copy couldn't be made. */
static int ptrace_copy_to_tracee(struct thread *target, uint64_t va, const void *kbuf, size_t len)
{
    const uint8_t *src = (const uint8_t *)kbuf;
    uint64_t pml4 = THREAD_PML4(target);
    while (len > 0) {
        uint64_t page_va = va & ~(PAGE_SIZE - 1);
        uint64_t off = va & (PAGE_SIZE - 1);
        if (ptrace_ensure_writable(target, page_va) < 0) return -1;
        uint64_t phys = vmm_virt_to_phys_in(pml4, page_va);
        if (!phys) return -1;
        size_t chunk = (size_t)(PAGE_SIZE - off);
        if (chunk > len) chunk = len;
        memcpy((void *)(phys + KERNEL_VMA + off), src, chunk);
        src += chunk; va += chunk; len -= chunk;
    }
    return 0;
}

/* Set or clear the TF (trap) flag on whichever saved-register copy the
 * target will actually resume from — see ptrace_trap_ctx's doc comment in
 * sched.h for why there are two. */
static void ptrace_set_tf(struct thread *target, int on)
{
    if (target->ptrace_trap_ctx) {
        if (on) target->ptrace_trap_ctx->rflags |= (1ULL << 8);
        else    target->ptrace_trap_ctx->rflags &= ~(1ULL << 8);
    } else {
        if (on) target->saved_user_rfl |= (1ULL << 8);
        else    target->saved_user_rfl &= ~(1ULL << 8);
    }
}

/* Find a thread this process is allowed to ptrace: it must be traced and
 * this thread must be its recorded tracer (PTRACE_TRACEME only — no
 * PTRACE_ATTACH, so the tracer is always the parent that forked it). */
static struct thread *ptrace_find_target(uint32_t tracer_tid, uint32_t pid)
{
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        struct thread *t = &thread_pool[i];
        if (t->pid == pid && t->ptrace_traced && t->ptrace_tracer_tid == tracer_tid)
            return t;
    }
    return NULL;
}

int64_t syscall_dispatch_proc(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg6;

    switch (nr) {
    case SYS_fork: {
        struct thread *child = sched_fork();
        child->saved_user_rdi = arg1;
        return (int64_t)child->tid;
    }

    case SYS_execve: {
        const char *path = (const char *)arg1;
        if (!path)
            return -22;
        SYSCALL_PROC_DEBUG_PRINT("SYS_execve ENTRY: arg1=%p path='%s' arg2=%p arg3=%p\n",
                                 (void *)arg1, path, (void *)arg2, (void *)arg3);

        char **user_argv = (char **)arg2;
        char **user_envp = (char **)arg3;

#define EXEC_MAX_ARGS  64
#define EXEC_MAX_ENVS  128
#define EXEC_MAX_STRSZ (32 * 1024)

        int argc = 0, envc = 0;
        size_t total_str_size = 0;

        if (user_argv) {
            for (; argc < EXEC_MAX_ARGS && user_argv[argc] != NULL; argc++)
                total_str_size += strlen(user_argv[argc]) + 1;
        }
        if (user_envp) {
            for (; envc < EXEC_MAX_ENVS && user_envp[envc] != NULL; envc++)
                total_str_size += strlen(user_envp[envc]) + 1;
        }
        if (total_str_size > EXEC_MAX_STRSZ)
            return -7;

        char *kstrbuf = NULL;
        const char *kargv[EXEC_MAX_ARGS + 1];
        const char *kenvp_k[EXEC_MAX_ENVS + 1];

        if (total_str_size > 0) {
            kstrbuf = (char *)kmalloc(total_str_size);
            if (!kstrbuf)
                return -12;
        }

        size_t koff = 0;
        for (int i = 0; i < argc; i++) {
            size_t len = strlen(user_argv[i]) + 1;
            kargv[i] = kstrbuf + koff;
            memcpy(kstrbuf + koff, user_argv[i], len);
            koff += len;
        }
        kargv[argc] = NULL;

        for (int i = 0; i < envc; i++) {
            size_t len = strlen(user_envp[i]) + 1;
            kenvp_k[i] = kstrbuf + koff;
            memcpy(kstrbuf + koff, user_envp[i], len);
            koff += len;
        }
        kenvp_k[envc] = NULL;

        int fd = vfs_open(path, 0, 0);
        if (fd < 0) {
            SYSCALL_PROC_DEBUG_PRINT("exec: cannot open '%s'\n", path);
            if (kstrbuf) kfree(kstrbuf);
            return -2;
        }

        /* Validate ELF magic before any destructive operations.
         * If the file is not a valid ELF, return ENOEXEC without touching
         * the current process image — critical for fork+exec patterns where
         * the child falls back to a second execve on failure. */
        {
            uint8_t magic[4];
            int nm = vfs_read(fd, magic, 4);
            if (nm != 4 || magic[0] != 0x7f || magic[1] != 'E' ||
                magic[2] != 'L'  || magic[3] != 'F') {
                vfs_close(fd);
                if (kstrbuf) kfree(kstrbuf);
                return -8; /* ENOEXEC */
            }
            /* Rewind so elf_load reads from the beginning. */
            struct thread *_cur = sched_current();
            THREAD_FDT(_cur)[fd].offset = 0;
        }

        /* Permission check: file must have at least one execute bit set. */
        {
            vfs_inode_info_t exec_info = {0};
            if (vfs_fstat(fd, &exec_info) == 0 &&
                exec_info.mode != 0 && !(exec_info.mode & 0111)) {
                vfs_close(fd);
                if (kstrbuf) kfree(kstrbuf);
                return -13;  /* EACCES */
            }
        }

        /* /proc/<pid> support: record comm (basename of path) and cmdline
         * (NUL-separated argv) now that exec is committed to proceeding. */
        {
            struct thread *_procinfo = sched_current();
            const char *base = path;
            for (const char *p = path; *p; p++)
                if (*p == '/') base = p + 1;
            size_t blen = strlen(base);
            if (blen >= sizeof(_procinfo->comm)) blen = sizeof(_procinfo->comm) - 1;
            memcpy(_procinfo->comm, base, blen);
            _procinfo->comm[blen] = '\0';

            uint32_t coff = 0;
            for (int i = 0; i < argc; i++) {
                size_t len = strlen(kargv[i]) + 1;
                if (coff + len > sizeof(_procinfo->cmdline))
                    len = sizeof(_procinfo->cmdline) - coff;
                if (len == 0) break;
                memcpy(_procinfo->cmdline + coff, kargv[i], len);
                coff += (uint32_t)len;
            }
            _procinfo->cmdline_len = coff;
        }

        /* Unmap old binary pages so elf_load writes fresh content.
         * Without this, a re-exec onto the same base address (ELF_PIE_BASE)
         * would find all pages already mapped and silently skip writing the
         * new binary — leaving the old image in place. */
        {
            struct thread *cur = sched_current();
            uint64_t old_top = (*THREAD_BRK_PTR(cur) > ELF_PIE_BASE)
                               ? ((*THREAD_BRK_PTR(cur) + 0xFFFULL) & ~0xFFFULL)
                               : (ELF_PIE_BASE + 0x4000000ULL); /* 64 MB fallback */
            /* Include the phdr-copy page that elf_load may have placed one
             * page below base. Real per-process address spaces (Phase 2)
             * mean every mapped page here — whether private or still CoW-
             * shared with a parent from fork() — is safe to pmm_unref_frame():
             * private pages (refcount 1) are freed; a still-shared page is
             * just decremented, leaving the parent's own reference intact. */
            for (uint64_t pg = ELF_PIE_BASE - 0x1000ULL; pg < old_top; pg += 0x1000ULL) {
                uint64_t phys = vmm_virt_to_phys(pg);
                if (!phys) continue;
                vmm_unmap_page(pg);
                pmm_unref_frame(phys);
            }
        }

        uint64_t entry = 0;
        uint64_t image_end = 0;
        uint64_t phdr_va = 0;
        uint16_t phnum = 0;
        int r = elf_load(fd, &entry, &image_end, &phdr_va, &phnum);
        vfs_close(fd);
        /* Real execve() failure semantics require the OLD image to still be
         * valid on error — but the unmap above already tore it down (elf_load
         * writes fresh pages at the same fixed VAs, so the old ones can't be
         * left mapped underneath it). If elf_load fails now (truncated file,
         * disk I/O error, corrupt ELF) there is no valid userspace image left
         * to return an error code to — returning here would resume execution
         * on unmapped/half-written code. Kill the process instead of risking
         * that undefined behavior. (Root cause of a real crash: BusyBox's
         * standalone-shell feature self-execs the same binary in place for
         * some invocations — e.g. `top` — with no fork, so a transient disk
         * read failure here previously corrupted the calling shell itself.) */
        if (r != 0) {
            /* Do not print @path here — it's a user-space pointer that may
             * already reference memory the unmap step above just freed. */
            SYSCALL_PROC_DEBUG_PRINT("exec: elf_load failed\n");
            if (kstrbuf) kfree(kstrbuf);
            printk("exec: failed to load after old image was torn down — killing process\n");
            sched_exit_current(-8);
            __builtin_unreachable();
        }
        if (entry == 0) {
            SYSCALL_PROC_DEBUG_PRINT("exec: ELF entry is 0 — rejecting\n");
            if (kstrbuf) kfree(kstrbuf);
            printk("exec: zero entry point after old image was torn down — killing process\n");
            sched_exit_current(-8);
            __builtin_unreachable();
        }

        struct thread *t = sched_current();
        SYSCALL_PROC_DEBUG_PRINT("SYS_execve: entry=%p stack=%p tid=%d\n",
                                 (void *)entry, (void *)t->user_stack_virt_top, t->tid);
        t->saved_user_rip = entry;

        size_t pointers_size = (size_t)(argc + 1 + envc + 1) * sizeof(char *);
        size_t block_size = sizeof(uint64_t) + pointers_size + 96 + total_str_size;
        block_size = (block_size + 15) & ~(size_t)15;

        uint64_t new_rsp = t->user_stack_virt_top - block_size;
        /* ASLR: add random 16-byte-aligned displacement below the arg block.
         * Every exec gets this now — real per-process address spaces (Phase 2)
         * mean there's no shared parent state to keep alignment with anymore. */
        new_rsp -= (proc_rdtsc() & 0xFFULL) * 16;
        uintptr_t sp = (uintptr_t)new_rsp;

        *(uint64_t *)sp = (uint64_t)argc;
        sp += sizeof(uint64_t);

        char **new_argv = (char **)sp;
        sp += (size_t)(argc + 1) * sizeof(char *);
        char **new_envp = (char **)sp;
        sp += (size_t)(envc + 1) * sizeof(char *);

        ((uint64_t *)sp)[0]  = 3;
        ((uint64_t *)sp)[1]  = phdr_va;
        ((uint64_t *)sp)[2]  = 4;
        ((uint64_t *)sp)[3]  = 56;
        ((uint64_t *)sp)[4]  = 5;
        ((uint64_t *)sp)[5]  = (uint64_t)phnum;
        ((uint64_t *)sp)[6]  = 6;
        ((uint64_t *)sp)[7]  = 4096;
        ((uint64_t *)sp)[8]  = 17;
        ((uint64_t *)sp)[9]  = 100;
        ((uint64_t *)sp)[10] = 0;
        ((uint64_t *)sp)[11] = 0;
        sp += 96;

        char *str_area = (char *)sp;
        size_t str_off = 0;

        for (int i = 0; i < argc; i++) {
            size_t len = strlen(kargv[i]) + 1;
            new_argv[i] = str_area + str_off;
            memcpy(str_area + str_off, kargv[i], len);
            str_off += len;
        }
        new_argv[argc] = NULL;

        for (int i = 0; i < envc; i++) {
            size_t len = strlen(kenvp_k[i]) + 1;
            new_envp[i] = str_area + str_off;
            memcpy(str_area + str_off, kenvp_k[i], len);
            str_off += len;
        }
        new_envp[envc] = NULL;

        if (kstrbuf)
            kfree(kstrbuf);

        t->saved_user_rsp = new_rsp;
        t->saved_user_rdi = (uint64_t)argc;
        t->saved_user_rsi = (uint64_t)new_argv;
        t->saved_user_rdx = (uint64_t)new_envp;
        t->saved_user_rbp = 0;
        t->saved_user_rbx = 0;
        t->saved_user_r12 = 0;
        t->saved_user_r13 = 0;
        t->saved_user_r14 = 0;
        t->saved_user_r15 = 0;
        /* execve always resets brk to the new image's end — matches real
         * POSIX semantics (the old heap belongs to the old, now-replaced,
         * program image). */
        *THREAD_BRK_PTR(t) = image_end;

        /* Reset mmap state: every mapped page in this process's own mmap
         * regions is either private or CoW-shared-with-a-parent (refcounted
         * since Phase 2) — pmm_unref_frame() handles both correctly (frees
         * private pages, just decrements still-shared ones). */
        mmap_region_t *exec_mmapr = THREAD_MMAPR(t);
        for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
            if (exec_mmapr[i].virt_addr == 0) continue;
            uint64_t rlen = (exec_mmapr[i].len + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
            uint64_t rbase = exec_mmapr[i].virt_addr;
            for (uint64_t pg = rbase; pg < rbase + rlen; pg += PAGE_SIZE) {
                uint64_t phys = vmm_virt_to_phys(pg);
                if (phys) {
                    vmm_unmap_page(pg);
                    pmm_unref_frame(phys);
                }
            }
            exec_mmapr[i].virt_addr = 0;
            exec_mmapr[i].len       = 0;
            exec_mmapr[i].prot      = 0;
        }
        *THREAD_MMAP_PTR(t) = MMAP_BASE - ((proc_rdtsc() >> 12) & 0xFFULL) * PAGE_SIZE;

        /* POSIX execve(): dispositions for caught signals reset to SIG_DFL
         * (a handler address is only meaningful in the old, now-replaced,
         * program image); SIG_IGN survives exec. Without this, a process
         * that forked from a parent with a real (non-default) handler
         * installed — e.g. a shell's SIGCHLD handler — inherits that stale
         * function pointer via sched_fork()'s signal_actions copy and later
         * jumps into garbage when the signal is actually delivered. */
        for (int i = 1; i <= 31; i++) {
            if (t->signal_actions[i].sa_handler != SIG_IGN) {
                t->signal_actions[i].sa_handler = SIG_DFL;
                t->signal_actions[i].sa_flags = 0;
            }
        }

        /* Zero FS.base so a timer between here and mlibc's wrfsbase doesn't
         * save a stale TLS pointer into ctx.fs_base. */
        __asm__ volatile("wrfsbase %0" :: "r"(0ULL));
        t->ctx.fs_base = 0;

        /* PTRACE_TRACEME: stop right after a successful exec, before the new
         * program's first instruction — the tracer's first wait4() catches
         * this, matching real ptrace's post-exec SIGTRAP stop. */
        if (t->ptrace_traced)
            sched_ptrace_stop(PTRACE_STOP_EXEC);

        return 0;
    }

    case SYS_waitpid:
    case SYS_wait4: {
        int64_t spid = (int64_t)arg1;
        uint64_t wstatus_va = arg2;
        struct thread *parent = sched_current();
        struct thread *child = NULL;

        if (spid == -1) {
            for (int i = 0; i < SCHED_MAX_THREADS; i++) {
                if (thread_pool[i].ppid == parent->pid &&
                    thread_pool[i].pid != 0 &&
                    thread_pool[i].state == THREAD_DEAD) {
                    child = &thread_pool[i];
                    break;
                }
            }
            if (!child) {
                /* Scan in reverse (highest TID first) so the most-recently
                 * forked child (e.g. `kill`) gets CPU before older siblings
                 * (e.g. `sleep`) — required for SIGALRM delivery ordering. */
                for (int i = SCHED_MAX_THREADS - 1; i >= 0; i--) {
                    if (thread_pool[i].ppid == parent->pid &&
                        thread_pool[i].pid != 0 &&
                        thread_pool[i].state != THREAD_DEAD) {
                        child = &thread_pool[i];
                        break;
                    }
                }
            }
        } else {
            uint32_t pid = (uint32_t)spid;
            for (int i = 0; i < SCHED_MAX_THREADS; i++) {
                if (thread_pool[i].pid == pid && thread_pool[i].ppid == parent->pid) {
                    child = &thread_pool[i];
                    break;
                }
            }
        }

        if (!child)
            return -10;

        /* WNOHANG (1): return 0 immediately instead of blocking if no child
         * status is available yet. WUNTRACED (2): also report (without
         * reaping) a child that stopped via SIGSTOP/SIGTSTP. Matches real
         * wait4/waitpid semantics. WNOHANG matters a lot more now that a
         * child can stay THREAD_STOPPED indefinitely (job control) — a
         * shell polling waitpid(-1, WNOHANG) to reap finished background
         * jobs must not be blocked forever by an unrelated stopped child. */
        int nohang   = ((uint64_t)arg3 & 1u) != 0;
        /* A ptrace-traced child's stops are always reported to its tracer —
         * real ptrace doesn't require WUNTRACED for that, since nobody but
         * the tracer would sensibly be waiting on a traced child anyway.
         * child->ptrace_traced is NOT snapshotted here: a tracer's wait4()
         * can be scheduled before its child has executed PTRACE_TRACEME
         * (the child sets the flag only just before its post-exec stop), so
         * the blocking loop below re-reads child->ptrace_traced on every
         * iteration instead of trusting a stale local. */
        int wuntraced = ((uint64_t)arg3 & 2u) != 0;

        int status = 0;
        int reap = 0;

        if (child->state == THREAD_DEAD) {
            status = wait_status_encode_exit(child->exit_code);
            reap = 1;
        } else if ((wuntraced || child->ptrace_traced) && child->state == THREAD_STOPPED) {
            status = wait_status_encode_stop(child->stop_signal);
        } else if (nohang) {
            return 0;
        } else {
            /* Real per-process address spaces (Phase 2): the child has been
             * THREAD_RUNNABLE in its own PML4 since sched_fork() returned —
             * no deferred activation/remap needed here anymore. */
            struct thread *cur = sched_current();
            cur->state = THREAD_WAITING;
            for (;;) {
                sched_yield();
                cur->state = THREAD_RUNNING;
                if (child->state == THREAD_DEAD) {
                    status = wait_status_encode_exit(child->exit_code);
                    reap = 1;
                    break;
                }
                if ((wuntraced || child->ptrace_traced) && child->state == THREAD_STOPPED) {
                    status = wait_status_encode_stop(child->stop_signal);
                    break;
                }
                if (syscall_has_pending_signal()) {
                    /* waitpid gets SA_RESTART (per research table) */
                    cur->syscall_restart_pending = 1;
                    return -4;  /* EINTR */
                }
                cur->state = THREAD_WAITING;
            }
        }

        if (wstatus_va != 0) {
            if (wstatus_va >= KERNEL_VMA)
                return (nr == SYS_waitpid) ? -14 : 0;
            *(int *)(uintptr_t)wstatus_va = status;
        }

        uint32_t child_pid = child->pid;
        if (reap)
            child->pid = 0;
        return (int64_t)child_pid;
    }

    case SYS_getpid:
        return (int64_t)sched_current()->pid;

    case SYS_getppid: {
        struct thread *t = sched_current();
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            if (thread_pool[i].tid == t->parent_tid) {
                if (thread_pool[i].state == THREAD_DEAD)
                    return 0;
                return (int64_t)thread_pool[i].pid;
            }
        }
        return 0;
    }

    case SYS_setsid: {
        struct thread *t = sched_current();
        if (t->pgid == t->pid)
            return -1;
        t->sid = t->pid;
        t->pgid = t->pid;
        return (int64_t)t->sid;
    }

    case SYS_setpgid: {
        struct thread *caller = sched_current();
        uint32_t target_pid = (arg1 > 0) ? (uint32_t)arg1 : caller->pid;
        uint32_t pgid = (uint32_t)arg2;
        if ((int64_t)arg2 < 0)
            return -22;

        struct thread *target = NULL;
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            if (thread_pool[i].pid == target_pid &&
                thread_pool[i].state != THREAD_DEAD) {
                target = &thread_pool[i];
                break;
            }
        }
        if (!target)
            return -3;
        if (target->sid != caller->sid)
            return -1;
        if (target->pid != caller->pid && target->ppid != caller->pid)
            return -13;

        if (pgid != 0 && pgid != target->pid) {
            int found_group = 0;
            for (int i = 0; i < SCHED_MAX_THREADS; i++) {
                if (thread_pool[i].pid == pgid &&
                    thread_pool[i].sid == target->sid &&
                    thread_pool[i].state != THREAD_DEAD) {
                    found_group = 1;
                    break;
                }
            }
            if (!found_group)
                return -1;
        }

        target->pgid = (pgid > 0) ? pgid : target->pid;
        return 0;
    }

    case SYS_kill: {
        int target_pid = (int)arg1;
        int sig = (int)arg2;
        if (sig < 1 || sig > 31)
            return -22;
        struct thread *target = NULL;
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            if (thread_pool[i].pid == (uint32_t)target_pid &&
                thread_pool[i].state != THREAD_DEAD) {
                target = &thread_pool[i];
                break;
            }
        }
        if (!target)
            return -3;
        sched_signal_thread(target, sig);
        return 0;
    }

    case SYS_signal: {
        int sig = (int)arg1;
        if (sig < 1 || sig > 31)
            return -22;
        if (sig == SIGKILL || sig == SIGTERM)
            return -22;
        struct thread *t = sched_current();
        mini_sigaction_t *act = &t->signal_actions[sig];
        sighandler_t old_handler = act->sa_handler;
        act->sa_handler = (sighandler_t)(uintptr_t)arg2;
        /* signal() sets SA_RESTART by default (Linux behaviour) unless SIG_DFL/IGN */
        if (act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN)
            act->sa_flags = 0;
        else
            act->sa_flags = (int)MINIOS_SA_RESTART;
        act->sa_mask = 0;
        return (int64_t)(uintptr_t)old_handler;
    }

    case SYS_sigaction: {
        int sig = (int)arg1;
        if (sig < 1 || sig > 64)
            return -22;  /* EINVAL */
        if (sig > 31)
            return -38;  /* ENOSYS: RT signals not yet supported; mlibc opts out gracefully */
        if (sig == SIGKILL || sig == SIGTERM)
            return -22;
        struct thread *t = sched_current();
        mini_sigaction_t *act = &t->signal_actions[sig];

        /* Newlib x86_64-elf (non-RTEMS) struct sigaction layout (NOT the same as mini_sigaction_t):
         * sys/signal.h uses the non-__rtems__ branch for bare-metal targets, which is:
         *   offset 0:  sa_handler  (function pointer, 8 bytes)
         *   offset 8:  sa_mask     (unsigned long, 8 bytes)
         *   offset 16: sa_flags    (int, 4 bytes)
         *   offset 20: _padding    (4 bytes)
         *   total: 24 bytes
         * Field-by-field copy is mandatory to avoid layout mismatch. */
        typedef struct {
            sighandler_t sa_handler;
            uint64_t     sa_mask;
            int          sa_flags;
            int          _pad;
        } user_sigaction_t;

        /* arg3 = old action output pointer */
        if (arg3 != 0) {
            /* WR-03: validate full struct fits in userspace before writing */
            if (arg3 + sizeof(user_sigaction_t) > KERNEL_VMA)
                return -14;  /* EFAULT */
            user_sigaction_t *old_out = (user_sigaction_t *)(uintptr_t)arg3;
            old_out->sa_handler = act->sa_handler;
            old_out->sa_mask    = (uint64_t)act->sa_mask;
            old_out->sa_flags   = act->sa_flags;
            old_out->_pad       = 0;
        }
        /* arg2 = new action input pointer */
        if (arg2 != 0) {
            /* WR-03: validate full struct fits in userspace before reading */
            if (arg2 + sizeof(user_sigaction_t) > KERNEL_VMA)
                return -14;  /* EFAULT */
            const user_sigaction_t *new_in = (const user_sigaction_t *)(uintptr_t)arg2;
            act->sa_handler = new_in->sa_handler;
            act->sa_flags   = new_in->sa_flags;
            act->sa_mask    = (uint32_t)new_in->sa_mask;
        }
        return 0;
    }

    case SYS_sigreturn: {
        struct thread *t = sched_current();
        /* Use the snapshot taken at ring3_invoke_handler() time, not the live
         * syscall_restart_* fields — those may have been overwritten by syscalls
         * made inside the signal handler (e.g. write()). */
        if (t->signal_ctx_restart_pending) {
            int last_sig = (int)t->syscall_last_signal;
            mini_sigaction_t *act = (last_sig >= 1 && last_sig <= 31)
                ? &t->signal_actions[last_sig] : NULL;
            t->signal_ctx_restart_pending = 0;
            if (act && (act->sa_flags & (int)MINIOS_SA_RESTART)) {
                /* WR-01: guard against unbounded recursion — if the restart would
                 * be immediately interrupted again (more signals pending) or if
                 * the saved syscall number is SYS_sigreturn itself, return EINTR
                 * instead of recursing, to prevent kernel stack overflow. */
                if (t->signal_ctx_restart_nr == SYS_sigreturn ||
                    syscall_has_pending_signal()) {
                    t->saved_user_rip = t->signal_ctx_restart_rip;
                    t->saved_user_rsp = t->signal_ctx_rsp;
                    return -4;  /* EINTR — let userspace decide */
                }
                /* Re-execute the interrupted syscall transparently (SA_RESTART) */
                t->saved_user_rsp = t->signal_ctx_rsp;
                return syscall_dispatch(
                    t->signal_ctx_restart_nr,
                    t->signal_ctx_restart_args[0],
                    t->signal_ctx_restart_args[1],
                    t->signal_ctx_restart_args[2],
                    t->signal_ctx_restart_args[3],
                    t->signal_ctx_restart_args[4],
                    t->signal_ctx_restart_args[5]);
            }
            /* No SA_RESTART: restore post-syscall RIP and return -EINTR */
            t->saved_user_rip = t->signal_ctx_restart_rip;
            t->saved_user_rsp = t->signal_ctx_rsp;
            return -4;  /* EINTR */
        }
        /* Normal signal return (handler not interrupting a syscall) */
        t->saved_user_rip = t->signal_ctx_rip;
        t->saved_user_rsp = t->signal_ctx_rsp;
        return 0;
    }

    case SYS_register_sigtrampoline: {
        /* arg1 = userspace VA of signal_trampoline; validate it is in user space */
        if (arg1 >= KERNEL_VMA)
            return -22;  /* EINVAL: trampoline must be in userspace */
        struct thread *t = sched_current();
        t->signal_trampoline_va = arg1;
        return 0;
    }

    case SYS_clone: {
        uint64_t flags       = arg1;
        uint64_t child_stack = arg2;
        uint32_t *parent_tid = (uint32_t *)arg3;
        uint32_t *child_tid  = (uint32_t *)arg4;
        uint64_t tls         = arg5;

        /* Only support CLONE_VM | CLONE_THREAD path (pthreads) */
        if (!(flags & 0x100)) /* CLONE_VM not set: fall back to fork */
            goto do_fork;

        struct thread *clone_child = sched_clone(child_stack, tls, child_tid);
        if (!clone_child) return -12; /* ENOMEM */
        if (parent_tid && (flags & 0x100000)) /* CLONE_PARENT_SETTID */
            *parent_tid = clone_child->tid;
        if (child_tid && (flags & 0x200000)) /* CLONE_CHILD_SETTID */
            *child_tid = clone_child->tid;
        if (flags & 0x400000) /* CLONE_CHILD_CLEARTID */
            clone_child->clear_tid_addr = (volatile uint32_t *)child_tid;
        return (int64_t)clone_child->tid;  /* parent gets child TID; child gets 0 */

      do_fork:;
        struct thread *fork_child = sched_fork();
        fork_child->saved_user_rdi = arg1;
        return (int64_t)fork_child->tid;
    }

    case SYS_futex: {
        volatile uint32_t *uaddr = (volatile uint32_t *)arg1;
        int  op    = (int)arg2 & ~(128 | 256);   /* strip PRIVATE and CLOCK flags */
        uint32_t val  = (uint32_t)arg3;
        struct thread *ft = sched_current();

        if (op == 0) { /* FUTEX_WAIT */
            if (*uaddr != val) return -11;  /* EAGAIN */
            ft->futex_uaddr = uaddr;
            ft->state = THREAD_FUTEX_WAIT;
            while (ft->state == THREAD_FUTEX_WAIT) {
                if (ft->pending_signals) {
                    ft->futex_uaddr = NULL;
                    ft->state = THREAD_RUNNING;
                    return -4;  /* EINTR */
                }
                sched_yield();
            }
            ft->futex_uaddr = NULL;
            return 0;
        } else if (op == 1) { /* FUTEX_WAKE */
            int woken = 0;
            int max_wake = (int)val;
            for (int i = 0; i < SCHED_MAX_THREADS && woken < max_wake; i++) {
                if (thread_pool[i].state == THREAD_FUTEX_WAIT &&
                    thread_pool[i].futex_uaddr == uaddr) {
                    thread_pool[i].futex_uaddr = NULL;
                    thread_pool[i].state = THREAD_RUNNABLE;
                    woken++;
                }
            }
            return (int64_t)woken;
        }
        return -38; /* ENOSYS for unimplemented ops */
    }

    case SYS_gettid:
        return (int64_t)sched_current()->tid;

    case SYS_set_tid_address: {
        volatile uint32_t *tidptr = (volatile uint32_t *)arg1;
        sched_current()->clear_tid_addr = tidptr;
        return (int64_t)sched_current()->tid;
    }

    case SYS_arch_prctl: {
        int code = (int)arg1;
        uint64_t addr = arg2;
        struct thread *apt = sched_current();
        if (code == 0x1002) { /* ARCH_SET_FS */
            apt->ctx.fs_base = addr;
            __asm__ volatile("wrfsbase %0" : : "r"(addr) : "memory");
            return 0;
        } else if (code == 0x1003) { /* ARCH_GET_FS */
            *(uint64_t *)addr = apt->ctx.fs_base;
            return 0;
        }
        return -22; /* EINVAL */
    }

    case SYS_exit:
    case 231:
        sched_exit_current((int)arg1);
        __builtin_unreachable();

    case SYS_nanosleep: {
        typedef struct { uint64_t tv_sec; uint64_t tv_nsec; } mini_timespec_t;
        mini_timespec_t *req = (mini_timespec_t *)(uintptr_t)arg1;
        mini_timespec_t *rmtp = (mini_timespec_t *)(uintptr_t)arg2;
        if (!req)
            return -22; /* EINVAL */

        uint64_t ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
        /* Saturate to 1 hour to prevent overflow */
        if (ms > 3600000)
            ms = 3600000;

        uint64_t timeout_tsc = ms * miniOS_tsc_hz / 1000;
        uint64_t deadline_tsc = proc_rdtsc() + timeout_tsc;

        while (proc_rdtsc() < deadline_tsc) {
            lwip_netif_poll();
            if (syscall_has_pending_signal()) {
                /* D-07: write remaining time to rmtp if non-NULL */
                /* CR-02: verify full struct fits below KERNEL_VMA before writing */
                if (rmtp != NULL &&
                    (uint64_t)(uintptr_t)rmtp + sizeof(mini_timespec_t) <= KERNEL_VMA) {
                    uint64_t remaining_tsc = deadline_tsc - proc_rdtsc();
                    rmtp->tv_sec  = remaining_tsc / miniOS_tsc_hz;
                    rmtp->tv_nsec = (remaining_tsc % miniOS_tsc_hz) * 1000000000ULL / miniOS_tsc_hz;
                }
                /* D-08: nanosleep does NOT get SA_RESTART — do not set restart_pending */
                return -4;  /* EINTR */
            }
            sched_yield();
        }
        return 0;
    }

    case SYS_sched_getcpu: {
        cpu_t *cpu = cpu_local();
        return (int64_t)cpu->cpu_id;
    }

    /* SYS_alarm_miniOS (453): arm ITIMER_REAL for @seconds seconds.
     * Returns remaining seconds of previous timer (0 if none), rounded up.
     * seconds=0 cancels the current timer. */
    case SYS_alarm_miniOS: {
        extern volatile uint64_t sched_tick_count;
        struct thread *t = sched_current();
        unsigned int seconds = (unsigned int)arg1;

        /* Compute remaining ticks of current timer */
        uint64_t remaining_ticks = 0;
        if (t->itimer_real_active && t->itimer_real_deadline_ticks > sched_tick_count)
            remaining_ticks = t->itimer_real_deadline_ticks - sched_tick_count;
        /* Convert to seconds, rounding up (1 tick = 10ms, 100 ticks = 1s) */
        unsigned int remaining_secs = (unsigned int)((remaining_ticks + 99) / 100);

        /* Arm or disarm */
        if (seconds == 0) {
            t->itimer_real_active = 0;
            t->itimer_real_deadline_ticks = 0;
            t->itimer_real_interval_ticks = 0;
        } else {
            uint64_t ticks = (uint64_t)seconds * 100;
            t->itimer_real_deadline_ticks = sched_tick_count + ticks;
            t->itimer_real_interval_ticks = 0;  /* one-shot */
            t->itimer_real_active = 1;
        }
        return (int64_t)remaining_secs;
    }

    /* SYS_setitimer_miniOS (452): set/get ITIMER_REAL via itimerval.
     * arg1=which (must be 0), arg2=new_value*, arg3=old_value* (may be NULL).
     * itimerval layout (x86-64): it_interval {tv_sec(8),tv_usec(8)}, it_value {tv_sec(8),tv_usec(8)}. */
    case SYS_setitimer_miniOS: {
        extern volatile uint64_t sched_tick_count;
        int which = (int)arg1;
        if (which != 0)
            return -22; /* EINVAL: only ITIMER_REAL supported */

        typedef struct {
            int64_t tv_sec;
            int64_t tv_usec;
        } mini_timeval_t;
        typedef struct {
            mini_timeval_t it_interval;
            mini_timeval_t it_value;
        } mini_itimerval_t;

        struct thread *t = sched_current();

        /* Write old value if requested */
        mini_itimerval_t *old = (mini_itimerval_t *)(uintptr_t)arg3;
        if (old != NULL) {
            if ((uint64_t)(uintptr_t)old + sizeof(mini_itimerval_t) > KERNEL_VMA)
                return -14; /* EFAULT */
            uint64_t remaining_ticks = 0;
            if (t->itimer_real_active && t->itimer_real_deadline_ticks > sched_tick_count)
                remaining_ticks = t->itimer_real_deadline_ticks - sched_tick_count;
            old->it_value.tv_sec  = (int64_t)(remaining_ticks / 100);
            old->it_value.tv_usec = (int64_t)((remaining_ticks % 100) * 10000);
            uint64_t intv = t->itimer_real_interval_ticks;
            old->it_interval.tv_sec  = (int64_t)(intv / 100);
            old->it_interval.tv_usec = (int64_t)((intv % 100) * 10000);
        }

        /* Apply new value if provided */
        mini_itimerval_t *nv = (mini_itimerval_t *)(uintptr_t)arg2;
        if (nv != NULL) {
            if ((uint64_t)(uintptr_t)nv + sizeof(mini_itimerval_t) > KERNEL_VMA)
                return -14; /* EFAULT */
            /* it_value == {0,0} disarms; otherwise arms */
            if (nv->it_value.tv_sec == 0 && nv->it_value.tv_usec == 0) {
                t->itimer_real_active = 0;
                t->itimer_real_deadline_ticks = 0;
                t->itimer_real_interval_ticks = 0;
            } else {
                uint64_t val_ticks = (uint64_t)nv->it_value.tv_sec * 100
                                   + (uint64_t)nv->it_value.tv_usec / 10000;
                if (val_ticks == 0) val_ticks = 1; /* at least one tick */
                uint64_t int_ticks = (uint64_t)nv->it_interval.tv_sec * 100
                                   + (uint64_t)nv->it_interval.tv_usec / 10000;
                t->itimer_real_deadline_ticks = sched_tick_count + val_ticks;
                t->itimer_real_interval_ticks = int_ticks;
                t->itimer_real_active = 1;
            }
        }
        return 0;
    }

    case SYS_ptrace: {
        long request = (long)arg1;
        uint32_t target_pid = (uint32_t)arg2;
        uint64_t addr = arg3;
        uint64_t data = arg4;
        struct thread *cur = sched_current();

        if (request == PTRACE_TRACEME) {
            cur->ptrace_traced = 1;
            cur->ptrace_tracer_tid = cur->parent_tid;
            return 0;
        }

        struct thread *target = ptrace_find_target(cur->tid, target_pid);
        if (!target)
            return -3; /* ESRCH */

        switch (request) {
        case PTRACE_PEEKTEXT:
        case PTRACE_PEEKDATA: {
            if (target->state != THREAD_STOPPED) return -3; /* ESRCH: not stopped */
            uint64_t word = 0;
            if (ptrace_copy_from_tracee(target, addr, &word, sizeof(word)) < 0)
                return -14; /* EFAULT */
            if (data != 0) {
                if (data >= KERNEL_VMA) return -14;
                *(uint64_t *)(uintptr_t)data = word;
                return 0;
            }
            return (int64_t)word; /* Linux glibc wrapper convention: return the word directly */
        }
        case PTRACE_POKETEXT:
        case PTRACE_POKEDATA: {
            if (target->state != THREAD_STOPPED) return -3;
            uint64_t word = data;
            if (ptrace_copy_to_tracee(target, addr, &word, sizeof(word)) < 0)
                return -14;
            return 0;
        }
        case PTRACE_GETREGS: {
            if (data >= KERNEL_VMA) return -14;
            struct miniOS_user_regs_struct regs;
            memset(&regs, 0, sizeof(regs));
            regs.rip    = target->saved_user_rip;
            regs.rsp    = target->saved_user_rsp;
            regs.eflags = target->saved_user_rfl;
            regs.rbp    = target->saved_user_rbp;
            regs.rbx    = target->saved_user_rbx;
            regs.r12    = target->saved_user_r12;
            regs.r13    = target->saved_user_r13;
            regs.r14    = target->saved_user_r14;
            regs.r15    = target->saved_user_r15;
            regs.cs     = USER_CS_SEL;
            regs.ss     = USER_DS_SEL;
            if (target->ptrace_stop_reason == PTRACE_STOP_SYSCALL_ENTRY ||
                target->ptrace_stop_reason == PTRACE_STOP_SYSCALL_EXIT) {
                regs.orig_rax = target->ptrace_syscall_nr;
                regs.rdi = target->syscall_restart_args[0];
                regs.rsi = target->syscall_restart_args[1];
                regs.rdx = target->syscall_restart_args[2];
                regs.r10 = target->syscall_restart_args[3];
                regs.r8  = target->syscall_restart_args[4];
                regs.r9  = target->syscall_restart_args[5];
                if (target->ptrace_stop_reason == PTRACE_STOP_SYSCALL_EXIT)
                    regs.rax = (uint64_t)target->ptrace_syscall_ret;
            }
            memcpy((void *)(uintptr_t)data, &regs, sizeof(regs));
            return 0;
        }
        case PTRACE_SETREGS: {
            if (data >= KERNEL_VMA) return -14;
            struct miniOS_user_regs_struct regs;
            memcpy(&regs, (void *)(uintptr_t)data, sizeof(regs));
            target->saved_user_rip = regs.rip;
            target->saved_user_rsp = regs.rsp;
            target->saved_user_rfl = regs.eflags;
            target->saved_user_rbp = regs.rbp;
            target->saved_user_rbx = regs.rbx;
            target->saved_user_r12 = regs.r12;
            target->saved_user_r13 = regs.r13;
            target->saved_user_r14 = regs.r14;
            target->saved_user_r15 = regs.r15;
            return 0;
        }
        case PTRACE_CONT:
            if (target->state != THREAD_STOPPED) return -3;
            ptrace_set_tf(target, 0);
            target->state = THREAD_RUNNABLE;
            return 0;
        case PTRACE_SINGLESTEP:
            if (target->state != THREAD_STOPPED) return -3;
            ptrace_set_tf(target, 1);
            target->state = THREAD_RUNNABLE;
            return 0;
        case PTRACE_SYSCALL:
            if (target->state != THREAD_STOPPED) return -3;
            target->ptrace_trace_syscalls = 1;
            target->state = THREAD_RUNNABLE;
            return 0;
        case PTRACE_KILL:
            if (target->state != THREAD_STOPPED) return -3;
            target->state = THREAD_RUNNABLE; /* let it run into signal_dispatch's SIGKILL path */
            sched_signal_thread(target, SIGKILL);
            return 0;
        default:
            return -22; /* EINVAL: unimplemented request */
        }
    }

    default:
        return SYSCALL_DISPATCH_UNHANDLED;
    }
}
