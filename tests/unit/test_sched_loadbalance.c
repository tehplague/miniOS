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

/* test_sched_loadbalance.c — unit tests for SCHED-02: load-balanced fork enqueue.
 * Inlines the sched_balance_enqueue algorithm to test in isolation.
 * stub_types.h force-included; do NOT redefine stdint types. */

#include "unity.h"
#include <miniOS/sched/sched.h>
#include <miniOS/arch/x86_64/smp.h>
#include <string.h>

/* ── Simulation state ── */
static struct thread idle0, idle1;
static struct thread worker;
static cpu_t         fake_cpus[2];

/* Track where the last enqueue landed */
static uint32_t last_enqueued_cpu;

/* Inline balance enqueue logic (no spinlock in host test) */
static void balance_enqueue_track(struct thread *t, cpu_t *cpus, uint32_t cpu_count)
{
    uint32_t best_cpu  = 0;
    uint32_t min_depth = cpus[0].queue_depth;
    for (uint32_t i = 1; i < cpu_count; i++) {
        if (cpus[i].queue_depth < min_depth) {
            min_depth = cpus[i].queue_depth;
            best_cpu  = i;
        }
    }
    last_enqueued_cpu = best_cpu;
    cpu_t *target                = &cpus[best_cpu];
    t->next                      = target->run_queue_tail->next;
    target->run_queue_tail->next = t;
    target->run_queue_tail       = t;
    target->queue_depth++;
}

void setUp(void) {
    memset(fake_cpus, 0, sizeof(fake_cpus));
    memset(&idle0,   0, sizeof(idle0));
    memset(&idle1,   0, sizeof(idle1));
    memset(&worker,  0, sizeof(worker));

    /* CPU0: circular idle sentinel, depth 0 */
    idle0.state                 = THREAD_RUNNING;
    idle0.next                  = &idle0;
    fake_cpus[0].run_queue_head = &idle0;
    fake_cpus[0].run_queue_tail = &idle0;
    fake_cpus[0].queue_depth    = 0;
    fake_cpus[0].idle_thread    = &idle0;
    fake_cpus[0].cpu_id         = 0;

    /* CPU1: same */
    idle1.state                 = THREAD_RUNNING;
    idle1.next                  = &idle1;
    fake_cpus[1].run_queue_head = &idle1;
    fake_cpus[1].run_queue_tail = &idle1;
    fake_cpus[1].queue_depth    = 0;
    fake_cpus[1].idle_thread    = &idle1;
    fake_cpus[1].cpu_id         = 1;

    worker.state = THREAD_BLOCKED;
}
void tearDown(void) { }

/* Helper: add N THREAD_RUNNABLE threads to a CPU's queue */
static struct thread extra[8];
static void fill_queue(cpu_t *cpu, uint32_t count) {
    for (uint32_t i = 0; i < count && i < 8; i++) {
        memset(&extra[i], 0, sizeof(extra[i]));
        extra[i].state               = THREAD_RUNNABLE;
        extra[i].next                = cpu->run_queue_tail->next;
        cpu->run_queue_tail->next    = &extra[i];
        cpu->run_queue_tail          = &extra[i];
        cpu->queue_depth++;
    }
}

/** Thread goes to CPU1 when CPU0 is more loaded. */
void test_balance_enqueue_picks_least_loaded(void)
{
    fill_queue(&fake_cpus[0], 3);  /* CPU0: depth=3 */
    fill_queue(&fake_cpus[1], 1);  /* CPU1: depth=1 */
    balance_enqueue_track(&worker, fake_cpus, 2);
    TEST_ASSERT_EQUAL_UINT32(1, last_enqueued_cpu);
    TEST_ASSERT_EQUAL_UINT32(2, fake_cpus[1].queue_depth);
}

/** When depths are equal, first CPU (index 0) is chosen (stable). */
void test_balance_enqueue_picks_cpu0_when_equal(void)
{
    fill_queue(&fake_cpus[0], 2);
    fill_queue(&fake_cpus[1], 2);
    balance_enqueue_track(&worker, fake_cpus, 2);
    TEST_ASSERT_EQUAL_UINT32(0, last_enqueued_cpu);
}

/** With a single CPU, thread always goes to CPU0. */
void test_balance_enqueue_single_cpu(void)
{
    fill_queue(&fake_cpus[0], 5);
    balance_enqueue_track(&worker, fake_cpus, 1);
    TEST_ASSERT_EQUAL_UINT32(0, last_enqueued_cpu);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_balance_enqueue_picks_least_loaded);
    RUN_TEST(test_balance_enqueue_picks_cpu0_when_equal);
    RUN_TEST(test_balance_enqueue_single_cpu);
    return UNITY_END();
}
