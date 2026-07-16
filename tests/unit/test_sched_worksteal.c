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

/* test_sched_worksteal.c — unit tests for SCHED-03: idle work stealing.
 * Inlines steal algorithm to test without LAPIC/spinlock dependencies.
 * stub_types.h force-included; do NOT redefine stdint types. */

#include "unity.h"
#include <miniOS/sched/sched.h>
#include <miniOS/arch/x86_64/smp.h>
#include <string.h>

static struct thread idle0, idle1;
static cpu_t         fake_cpus[2];
#define FAKE_MAX_CPUS 2

/* Count threads in a circular list (including the start node) */
static uint32_t count_circular(struct thread *head)
{
    if (!head) return 0;
    uint32_t       n = 1;
    struct thread *t = head->next;
    while (t != head) { n++; t = t->next; }
    return n;
}

/* Inline work-steal algorithm (without spinlock) */
static void steal_sim(cpu_t *local_cpu, cpu_t *cpus, uint32_t cpu_count)
{
    if (local_cpu->queue_depth > 0)
        return;

    uint32_t busiest_cpu = FAKE_MAX_CPUS;   /* sentinel */
    uint32_t max_depth   = 1;               /* need > 1 to steal */
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (&cpus[i] == local_cpu) continue;
        if (cpus[i].queue_depth > max_depth) {
            max_depth   = cpus[i].queue_depth;
            busiest_cpu = i;
        }
    }
    if (busiest_cpu == FAKE_MAX_CPUS) return;

    cpu_t    *victim      = &cpus[busiest_cpu];
    uint32_t  steal_count = victim->queue_depth / 2;

    struct thread *steal_head = victim->run_queue_head;
    struct thread *steal_tail = steal_head;
    for (uint32_t i = 1; i < steal_count; i++)
        steal_tail = steal_tail->next;

    struct thread *new_victim_head = steal_tail->next;
    if (new_victim_head == victim->run_queue_head) {
        /* Stole everything: victim becomes idle-only */
        victim->run_queue_head        = victim->idle_thread;
        victim->run_queue_tail        = victim->idle_thread;
        victim->idle_thread->next     = victim->idle_thread;
    } else {
        victim->run_queue_head        = new_victim_head;
        victim->run_queue_tail->next  = new_victim_head;  /* close victim's circle */
    }
    victim->queue_depth -= steal_count;

    steal_tail->next                 = local_cpu->run_queue_tail->next;  /* = local head */
    local_cpu->run_queue_tail->next  = steal_head;
    local_cpu->run_queue_tail        = steal_tail;
    local_cpu->queue_depth          += steal_count;
}

/* Helper: add N THREAD_RUNNABLE threads to a CPU's queue */
static struct thread workers[8];
static void fill_queue(cpu_t *cpu, uint32_t count) {
    for (uint32_t i = 0; i < count && i < 8; i++) {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].state              = THREAD_RUNNABLE;
        workers[i].next               = cpu->run_queue_tail->next;
        cpu->run_queue_tail->next     = &workers[i];
        cpu->run_queue_tail           = &workers[i];
        cpu->queue_depth++;
    }
}

void setUp(void) {
    memset(fake_cpus, 0, sizeof(fake_cpus));
    memset(&idle0,    0, sizeof(idle0));
    memset(&idle1,    0, sizeof(idle1));

    idle0.state = THREAD_RUNNING; idle0.next = &idle0;
    fake_cpus[0].run_queue_head = &idle0;
    fake_cpus[0].run_queue_tail = &idle0;
    fake_cpus[0].queue_depth    = 0;
    fake_cpus[0].idle_thread    = &idle0;
    fake_cpus[0].cpu_id         = 0;

    idle1.state = THREAD_RUNNING; idle1.next = &idle1;
    fake_cpus[1].run_queue_head = &idle1;
    fake_cpus[1].run_queue_tail = &idle1;
    fake_cpus[1].queue_depth    = 0;
    fake_cpus[1].idle_thread    = &idle1;
    fake_cpus[1].cpu_id         = 1;
}
void tearDown(void) { }

/** CPU0 empty + CPU1 has 4: steal 2 each. */
void test_work_steal_moves_half(void)
{
    fill_queue(&fake_cpus[1], 4);
    steal_sim(&fake_cpus[0], fake_cpus, 2);
    TEST_ASSERT_EQUAL_UINT32(2, fake_cpus[0].queue_depth);
    TEST_ASSERT_EQUAL_UINT32(2, fake_cpus[1].queue_depth);
}

/** CPU0 has work: steal_sim must not change anything. */
void test_work_steal_noop_when_local_has_work(void)
{
    fill_queue(&fake_cpus[0], 1);
    fill_queue(&fake_cpus[1], 4);
    steal_sim(&fake_cpus[0], fake_cpus, 2);
    TEST_ASSERT_EQUAL_UINT32(1, fake_cpus[0].queue_depth);
    TEST_ASSERT_EQUAL_UINT32(4, fake_cpus[1].queue_depth);
}

/** No CPU has queue_depth > 1: steal is a no-op. */
void test_work_steal_noop_when_no_victims(void)
{
    fill_queue(&fake_cpus[1], 1);   /* depth==1, not > 1 */
    steal_sim(&fake_cpus[0], fake_cpus, 2);
    TEST_ASSERT_EQUAL_UINT32(0, fake_cpus[0].queue_depth);
    TEST_ASSERT_EQUAL_UINT32(1, fake_cpus[1].queue_depth);
}

/** After stealing 2 from 4, CPU0 circular list has idle + 2 workers = 3 nodes. */
void test_work_steal_circular_invariant_after_steal(void)
{
    fill_queue(&fake_cpus[1], 4);
    steal_sim(&fake_cpus[0], fake_cpus, 2);
    /* CPU0: idle sentinel + 2 stolen workers = 3 total nodes */
    uint32_t n = count_circular(fake_cpus[0].run_queue_head);
    TEST_ASSERT_EQUAL_UINT32(3, n);
    /* Circularity: tail->next == head */
    TEST_ASSERT_EQUAL_PTR(fake_cpus[0].run_queue_head,
                          fake_cpus[0].run_queue_tail->next);
    /* CPU1: idle sentinel + 2 remaining workers = 3 total nodes */
    n = count_circular(fake_cpus[1].run_queue_head);
    TEST_ASSERT_EQUAL_UINT32(3, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_work_steal_moves_half);
    RUN_TEST(test_work_steal_noop_when_local_has_work);
    RUN_TEST(test_work_steal_noop_when_no_victims);
    RUN_TEST(test_work_steal_circular_invariant_after_steal);
    return UNITY_END();
}
