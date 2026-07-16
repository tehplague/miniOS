// MIT License
// Copyright (c) 2026 Christian Spoo

/*
 * testfutex — direct futex syscall tests (no pthreads dependency)
 *
 * Tests:
 *   futex_wake_nowaiters:  FUTEX_WAKE with no waiters returns 0
 *   futex_wait_eagain:     FUTEX_WAIT with mismatched value returns EAGAIN immediately
 *   futex_wait_wake:       clone thread: child FUTEX_WAITs, parent FUTEX_WAKEs after write;
 *                          join via CLONE_CHILD_CLEARTID futex
 *   gettid:                SYS_gettid returns a non-zero value
 */

#include <unistd.h>
#include <string.h>
#include <stdint.h>

/* Raw syscall numbers */
#define SYS_futex         202
#define SYS_gettid        186
#define SYS_clone_nr       56

/* Futex ops */
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_PRIVATE     128

/* Clone flags */
#define CLONE_VM          0x00000100
#define CLONE_FS          0x00000200
#define CLONE_SIGHAND     0x00000800
#define CLONE_THREAD      0x00010000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_CHILD_CLEARTID 0x00400000

static long futex(uint32_t *uaddr, int op, uint32_t val) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "0"((long)SYS_futex), "D"(uaddr), "S"((long)op), "d"((long)val),
          "r"((long)0), "r"((long)0)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long sys_gettid(void) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"((long)SYS_gettid) : "rcx", "r11", "memory");
    return ret;
}

static void write_str(const char *s) { write(1, s, strlen(s)); }

static void test_futex_wake_nowaiters(void) {
    uint32_t val = 0;
    long r = futex(&val, FUTEX_WAKE | FUTEX_PRIVATE, 1);
    if (r == 0)
        write_str("PASS: futex_wake_nowaiters\n");
    else
        write_str("FAIL: futex_wake_nowaiters: expected 0\n");
}

static void test_futex_wait_eagain(void) {
    uint32_t val = 42;
    /* FUTEX_WAIT with val != *uaddr should return -EAGAIN immediately */
    long r = futex(&val, FUTEX_WAIT | FUTEX_PRIVATE, 99);
    if (r == -11)  /* -EAGAIN */
        write_str("PASS: futex_wait_eagain\n");
    else
        write_str("FAIL: futex_wait_eagain: expected -EAGAIN\n");
}

static void test_futex_wait_wake(void) {
    /* Thread-shared variables: clone(CLONE_VM) shares address space so both
     * threads see the same physical memory at these virtual addresses. */
    static volatile uint32_t shared = 1; /* futex word child waits on */
    static volatile uint32_t ctid   = 0; /* set to child TID by CLONE_CHILD_SETTID;
                                          * cleared to 0 on child exit (CLONE_CHILD_CLEARTID) */
    static uint8_t clone_stk[4096];

    shared = 1;
    ctid   = 0;

    /* Align stack top to 16 bytes; place a null sentinel return address. */
    uint64_t stk_top = ((uint64_t)(clone_stk + sizeof(clone_stk))) & ~15UL;
    *((volatile uint64_t *)(stk_top - 8)) = 0;
    stk_top -= 8;

    unsigned long clone_flags = CLONE_VM | CLONE_FS | CLONE_SIGHAND | CLONE_THREAD |
                                CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;

    long tid;
    /* SYS_clone(flags, child_stack, parent_tid=NULL, child_tid=&ctid, tls=0).
     * Linux x86-64 ABI: arg4 passes in r10 (rcx is clobbered by syscall).  */
    register long r10_ctid __asm__("r10") = (long)(uint32_t *)&ctid;
    __asm__ volatile(
        "syscall"
        : "=a"(tid)
        : "0"((long)SYS_clone_nr), "D"(clone_flags), "S"(stk_top),
          "d"(0L), "r"(r10_ctid)
        : "rcx", "r11", "memory"
    );

    if (tid < 0) {
        write_str("FAIL: futex_wait_wake: clone() failed\n");
        return;
    }
    if (tid == 0) {
        /* Child thread: block until parent sets shared = 0 and wakes us. */
        long r = futex((uint32_t *)&shared, FUTEX_WAIT, 1);
        /* r == 0: woken by parent; r == -11 (EAGAIN): parent already wrote 0 */
        if (r == 0 || r == -11)
            write_str("PASS: futex_wait_wake (child woke)\n");
        else
            write_str("FAIL: futex_wait_wake: unexpected futex return\n");
        _exit(0);
        __builtin_unreachable();
    }

    /* Parent: spin to give child time to reach futex(WAIT), then wake it. */
    volatile int spin = 500000;
    while (spin-- > 0) __asm__ volatile("pause");

    shared = 0;
    futex((uint32_t *)&shared, FUTEX_WAKE, 1);

    /* Join via CLONE_CHILD_CLEARTID: when child exits the kernel writes
     * ctid = 0 and does futex(WAKE, &ctid).  We wait until that happens. */
    uint32_t join_tid = ctid;   /* non-zero TID set by CLONE_CHILD_SETTID */
    if (join_tid == 0) {
        write_str("FAIL: futex_wait_wake: ctid never set\n");
        return;
    }
    while (ctid != 0) {
        long r = futex((uint32_t *)&ctid, FUTEX_WAIT, join_tid);
        if (r == -11) break;  /* EAGAIN: child already exited */
    }
    write_str("PASS: futex_wait_wake (parent joined)\n");
}

static void test_gettid(void) {
    long tid = sys_gettid();
    if (tid > 0)
        write_str("PASS: gettid\n");
    else
        write_str("FAIL: gettid: expected positive tid\n");
}

int main(void) {
    write_str("=== testfutex ===\n");
    test_futex_wake_nowaiters();
    test_futex_wait_eagain();
    test_futex_wait_wake();
    test_gettid();
    write_str("=== testfutex done ===\n");
    return 0;
}
