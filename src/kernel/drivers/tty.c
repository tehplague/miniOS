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

#include <miniOS/drivers/tty.h>

#include <miniOS/arch/x86_64/spinlock.h>
#include <miniOS/drivers/vt.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/sched/sched.h>
#include <string.h>

/* Any userspace pointer must be below the kernel virtual address window.
 * Addresses >= KERNEL_VMA are kernel-only and must never be dereferenced on
 * behalf of userspace. */
#define USER_PTR_VALID(p) ((uint64_t)(uintptr_t)(p) < KERNEL_VMA)

#ifndef SIGINT
#define SIGINT 2
#endif

#define TTY_BUFFER_SIZE 256

typedef struct {
    spinlock_t lock;
    struct minios_termios termios;
    char canonical_buf[TTY_BUFFER_SIZE];
    uint16_t canonical_len;
    char input_buf[TTY_BUFFER_SIZE];
    uint16_t input_head;
    uint16_t input_tail;
    uint8_t eof_pending;
    /* Explicit foreground process-group contract.
     * Set by TIOCSPGRP / tcsetpgrp(); zero means fall back to
     * sched_current() session/group targeting. */
    uint32_t foreground_pgid;
    uint32_t foreground_sid;
} tty_state_t;

static tty_state_t tty_state = {
    .lock = SPINLOCK_INIT,
};

static uint16_t tty_ring_count(void) {
    return (uint16_t)(tty_state.input_head - tty_state.input_tail);
}

static void tty_ring_push(char c) {
    if (tty_ring_count() >= TTY_BUFFER_SIZE) {
        return;
    }

    tty_state.input_buf[tty_state.input_head & (TTY_BUFFER_SIZE - 1)] = c;
    tty_state.input_head++;
}

static int tty_ring_pop(char *out) {
    if (tty_state.input_head == tty_state.input_tail) {
        return 0;
    }

    *out = tty_state.input_buf[tty_state.input_tail & (TTY_BUFFER_SIZE - 1)];
    tty_state.input_tail++;
    return 1;
}

static void tty_echo_byte(char c) {
    if ((tty_state.termios.c_lflag & TTY_LFLAG_ECHO) == 0) {
        return;
    }

    vt_write_byte(c);
}

static void tty_flush_pending_input_locked(void) {
    tty_state.input_head = 0;
    tty_state.input_tail = 0;
    tty_state.eof_pending = 0;
}

static void tty_finalize_canonical_line_locked(void) {
    for (uint16_t i = 0; i < tty_state.canonical_len; i++) {
        tty_ring_push(tty_state.canonical_buf[i]);
    }
    tty_state.canonical_len = 0;
}

static void tty_deliver_sigint_locked(void) {
    /*
     * When an explicit foreground pgid is set via TIOCSPGRP (job-control
     * shells), signal every thread in that group.  Without job control
     * (CONFIG_ASH_JOB_CONTROL=n), ash never calls tcsetpgrp(), so
     * foreground_pgid stays 0.  In that case signal only sched_current() —
     * the thread that is actively running — because ash and its children
     * share the same pgid and a pgid sweep would hit both.
     */
    if (tty_state.foreground_pgid != 0) {
        /* Explicit job-control foreground: signal the whole foreground pgid,
         * including sleeping members (POSIX requires delivery regardless of
         * blocking state when the target is explicit). */
        uint32_t target_pgid = tty_state.foreground_pgid;
        uint32_t target_sid  = tty_state.foreground_sid;
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            struct thread *t = &thread_pool[i];
            if (t->state == THREAD_DEAD || t->pid == 0)
                continue;
            if (target_sid  != 0 && t->sid  != target_sid)
                continue;
            if (target_pgid != 0 && t->pgid != target_pgid)
                continue;
            sched_signal_thread(t, SIGINT);
        }
    } else {
        /* No job control (CONFIG_ASH_JOB_CONTROL=n): ash never calls
         * tcsetpgrp(), so all processes share one pgid.  Signal only
         * THREAD_RUNNING/THREAD_READY userspace threads — this catches the
         * foreground child (poll loop, sched_yield()) while skipping ash
         * itself which is sleeping in wait() via hlt (THREAD_WAITING).
         * Signalling a THREAD_WAITING ash would cause it to handle SIGINT
         * and exit, killing the interactive shell. */
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            struct thread *t = &thread_pool[i];
            if (t->state == THREAD_DEAD || t->state == THREAD_WAITING || t->pid == 0)
                continue;
            sched_signal_thread(t, SIGINT);
        }
    }
}

static void tty_apply_defaults_locked(void) {
    memset(&tty_state.termios, 0, sizeof(tty_state.termios));
    tty_state.termios.c_lflag = TTY_LFLAG_ICANON | TTY_LFLAG_ECHO | TTY_LFLAG_ISIG;
    tty_state.termios.c_cc[VINTR] = 0x03;
    tty_state.termios.c_cc[VEOF] = 0x04;
    tty_state.canonical_len = 0;
    tty_flush_pending_input_locked();
}

static void tty_apply_termios_locked(const struct minios_termios *termios, int flush_input) {
    tty_state.termios = *termios;
    tty_state.termios.c_lflag &= (TTY_LFLAG_ICANON | TTY_LFLAG_ECHO | TTY_LFLAG_ISIG);
    tty_state.canonical_len = 0;
    if (flush_input)
        tty_flush_pending_input_locked();
}

void tty_init(void) {
    unsigned long flags;

    spinlock_irqsave(&tty_state.lock, &flags);
    tty_apply_defaults_locked();
    spinlock_irqrestore(&tty_state.lock, flags);
}

int tty_has_data(void) {
    unsigned long flags;
    spinlock_irqsave(&tty_state.lock, &flags);
    int avail = (tty_state.input_head != tty_state.input_tail);
    spinlock_irqrestore(&tty_state.lock, flags);
    return avail;
}

void tty_inject_escape(const char *seq, int len) {
    unsigned long flags;
    spinlock_irqsave(&tty_state.lock, &flags);
    for (int i = 0; i < len; i++)
        tty_ring_push(seq[i]);
    spinlock_irqrestore(&tty_state.lock, flags);
}

void tty_keyboard_input(char c) {
    unsigned long flags;
    uint32_t lflag;

    spinlock_irqsave(&tty_state.lock, &flags);

    lflag = tty_state.termios.c_lflag;
    if ((lflag & TTY_LFLAG_ICANON) == 0) {
        tty_ring_push(c);
        spinlock_irqrestore(&tty_state.lock, flags);
        return;
    }

    if ((lflag & TTY_LFLAG_ISIG) != 0 && c == (char)tty_state.termios.c_cc[VINTR]) {
        tty_state.canonical_len = 0;
        tty_deliver_sigint_locked();
        spinlock_irqrestore(&tty_state.lock, flags);
        return;
    }

    if (c == '\b') {
        if (tty_state.canonical_len != 0) {
            /* Remove one full UTF-8 character: strip trailing continuation bytes
             * (10xxxxxx) then the lead byte. */
            while (tty_state.canonical_len > 0 &&
                   ((uint8_t)tty_state.canonical_buf[tty_state.canonical_len - 1] & 0xC0) == 0x80)
                tty_state.canonical_len--;
            if (tty_state.canonical_len > 0)
                tty_state.canonical_len--;
            tty_echo_byte('\b');
            tty_echo_byte(' ');
            tty_echo_byte('\b');
        }
        spinlock_irqrestore(&tty_state.lock, flags);
        return;
    }

    if (c == (char)tty_state.termios.c_cc[VEOF]) {
        if (tty_state.canonical_len == 0) {
            tty_state.eof_pending = 1;
        } else {
            tty_finalize_canonical_line_locked();
        }
        spinlock_irqrestore(&tty_state.lock, flags);
        return;
    }

    if (c == '\n') {
        if (tty_state.canonical_len < TTY_BUFFER_SIZE) {
            tty_state.canonical_buf[tty_state.canonical_len++] = '\n';
        }
        tty_echo_byte('\n');
        tty_finalize_canonical_line_locked();
        spinlock_irqrestore(&tty_state.lock, flags);
        return;
    }

    if (tty_state.canonical_len < TTY_BUFFER_SIZE) {
        tty_state.canonical_buf[tty_state.canonical_len++] = c;
        tty_echo_byte(c);
    }

    spinlock_irqrestore(&tty_state.lock, flags);
}

int tty_read(void *buf, uint32_t len, int nonblock) {
    char *out = (char *)buf;
    uint32_t copied = 0;

    if (!buf || len == 0) {
        return 0;
    }

    for (;;) {
        unsigned long flags;

        spinlock_irqsave(&tty_state.lock, &flags);
        while (copied < len && tty_ring_pop(&out[copied])) {
            copied++;
        }

        if (copied != 0) {
            spinlock_irqrestore(&tty_state.lock, flags);
            return (int)copied;
        }

        if (tty_state.eof_pending) {
            tty_state.eof_pending = 0;
            spinlock_irqrestore(&tty_state.lock, flags);
            return 0;
        }

        spinlock_irqrestore(&tty_state.lock, flags);
        if (nonblock) {
            return -11; /* -EAGAIN */
        }

#ifndef TEST_BUILD
        __asm__ volatile("sti; hlt" ::: "memory");
#else
        return 0;
#endif
    }
}

int tty_write(const void *buf, uint32_t len) {
    const char *bytes = (const char *)buf;

    for (uint32_t i = 0; i < len; i++) {
        vt_write_byte(bytes[i]);
    }

    return (int)len;
}

int tty_ioctl(uint64_t cmd, uint64_t arg) {
    unsigned long flags;

    switch (cmd) {
        case TIOCGWINSZ: {
            struct minios_winsize *ws = (struct minios_winsize *)(uintptr_t)arg;
            if (!ws || !USER_PTR_VALID(ws)) return -14; /* EFAULT */
            vt_get_winsize(&ws->ws_row, &ws->ws_col);
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }
        case TCGETS: {
            struct minios_termios *termios = (struct minios_termios *)(uintptr_t)arg;
            if (!termios || !USER_PTR_VALID(termios)) return -14; /* EFAULT */
            spinlock_irqsave(&tty_state.lock, &flags);
            *termios = tty_state.termios;
            spinlock_irqrestore(&tty_state.lock, flags);
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            const struct minios_termios *termios = (const struct minios_termios *)(uintptr_t)arg;
            if (!termios || !USER_PTR_VALID(termios)) return -14; /* EFAULT */
            spinlock_irqsave(&tty_state.lock, &flags);
            tty_apply_termios_locked(termios, cmd != TCSETS);
            spinlock_irqrestore(&tty_state.lock, flags);
            return 0;
        }
        case TIOCSPGRP: {
            /* Set the tty foreground process group.  @arg is a pid_t *. */
            const int *pgid_ptr = (const int *)(uintptr_t)arg;
            if (!pgid_ptr || !USER_PTR_VALID(pgid_ptr)) return -14; /* EFAULT */
            int pgid = *pgid_ptr;
            if (pgid < 0) return -22;
            struct thread *caller = sched_current();
            spinlock_irqsave(&tty_state.lock, &flags);
            tty_state.foreground_pgid = (uint32_t)pgid;
            tty_state.foreground_sid  = caller ? caller->sid : 0;
            spinlock_irqrestore(&tty_state.lock, flags);
            return 0;
        }
        case TIOCGPGRP: {
            /* Return the tty foreground process group.  @arg is a pid_t *. */
            int *pgid_ptr = (int *)(uintptr_t)arg;
            if (!pgid_ptr || !USER_PTR_VALID(pgid_ptr)) return -14; /* EFAULT */
            spinlock_irqsave(&tty_state.lock, &flags);
            *pgid_ptr = (int)tty_state.foreground_pgid;
            spinlock_irqrestore(&tty_state.lock, flags);
            return 0;
        }
        case TIOCSCTTY:
        case TIOCNOTTY:
            return 0;
        default:
            return -25;
    }
}

int tty_isatty_fd(int fd) {
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}
