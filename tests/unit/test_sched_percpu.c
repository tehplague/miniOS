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

/* test_sched_percpu.c — unit tests for per-CPU run queue (SCHED-01).
 * Tests the queue data structure and sched_next algorithm in isolation.
 * Does NOT compile sched.c (it pulls in x86_64 asm); tests the algorithm directly.
 * stub_types.h is force-included via -include flag; do NOT redefine stdint types. */

#include "unity.h"
#include <miniOS/sched/sched.h>   /* struct thread, thread_state_t */
#include <miniOS/arch/x86_64/smp.h> /* cpu_t, cpu_t fields */
#include <string.h>

/* ── Helpers: inline the queue algorithm so we can test it without asm ── */

/* sched_next_from_head: walk a per-CPU circular list from head.
 * Returns first THREAD_RUNNABLE node, or idle_thread if none found. */
static struct thread *sched_next_from_head(struct thread *head,
                                            struct thread *idle_thread)
{
    struct thread *t     = head;
    struct thread *start = t;
    do {
        if (t->state == THREAD_RUNNABLE)
            return t;
        t = t->next;
    } while (t != start);
    return idle_thread;
}

void setUp(void)    { }
void tearDown(void) { }

/* ── Test fixtures ── */

static struct thread idle_t;
static struct thread worker_t;
static cpu_t         fake_cpu;

static void setup_idle_only(void)
{
    memset(&idle_t,   0, sizeof(idle_t));
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    idle_t.state        = THREAD_RUNNING;
    idle_t.next         = &idle_t;     /* circular: self-link */
    fake_cpu.run_queue_head = &idle_t;
    fake_cpu.run_queue_tail = &idle_t;
    fake_cpu.queue_depth    = 0;
    fake_cpu.idle_thread    = &idle_t;
}

/* ── SCHED-01 tests ── */

/** Idle-only queue: head == tail == idle, depth == 0. */
void test_percpu_queue_init(void)
{
    setup_idle_only();
    TEST_ASSERT_EQUAL_PTR(&idle_t, fake_cpu.run_queue_head);
    TEST_ASSERT_EQUAL_PTR(&idle_t, fake_cpu.run_queue_tail);
    TEST_ASSERT_EQUAL_UINT32(0, fake_cpu.queue_depth);
}

/** After tail-inserting a second thread, depth == 1 and circularity holds. */
void test_percpu_enqueue_increases_depth(void)
{
    setup_idle_only();
    memset(&worker_t, 0, sizeof(worker_t));
    worker_t.state = THREAD_RUNNABLE;

    /* Tail insert: tail->next was idle (head); new node goes after tail */
    worker_t.next                        = fake_cpu.run_queue_tail->next; /* = head */
    fake_cpu.run_queue_tail->next        = &worker_t;
    fake_cpu.run_queue_tail              = &worker_t;
    fake_cpu.queue_depth++;

    TEST_ASSERT_EQUAL_UINT32(1, fake_cpu.queue_depth);
    /* Circular invariant: tail->next == head */
    TEST_ASSERT_EQUAL_PTR(fake_cpu.run_queue_head, fake_cpu.run_queue_tail->next);
}

/** sched_next returns the RUNNABLE worker when idle is THREAD_RUNNING. */
void test_percpu_sched_next_returns_runnable(void)
{
    setup_idle_only();
    memset(&worker_t, 0, sizeof(worker_t));
    worker_t.state                       = THREAD_RUNNABLE;
    worker_t.next                        = fake_cpu.run_queue_tail->next;
    fake_cpu.run_queue_tail->next        = &worker_t;
    fake_cpu.run_queue_tail              = &worker_t;
    fake_cpu.queue_depth++;

    struct thread *next = sched_next_from_head(fake_cpu.run_queue_head,
                                               fake_cpu.idle_thread);
    TEST_ASSERT_EQUAL_PTR(&worker_t, next);
}

/** When all threads are THREAD_DEAD, sched_next falls back to idle. */
void test_percpu_sched_next_falls_back_to_idle(void)
{
    setup_idle_only();
    memset(&worker_t, 0, sizeof(worker_t));
    worker_t.state                       = THREAD_DEAD;
    worker_t.next                        = fake_cpu.run_queue_tail->next;
    fake_cpu.run_queue_tail->next        = &worker_t;
    fake_cpu.run_queue_tail              = &worker_t;
    fake_cpu.queue_depth++;

    /* Also make idle THREAD_DEAD for a worst-case scan */
    idle_t.state = THREAD_DEAD;

    struct thread *next = sched_next_from_head(fake_cpu.run_queue_head,
                                               fake_cpu.idle_thread);
    TEST_ASSERT_EQUAL_PTR(&idle_t, next);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_percpu_queue_init);
    RUN_TEST(test_percpu_enqueue_increases_depth);
    RUN_TEST(test_percpu_sched_next_returns_runnable);
    RUN_TEST(test_percpu_sched_next_falls_back_to_idle);
    return UNITY_END();
}
