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

#include <miniOS/sched/sched.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/arch/x86_64/spinlock.h>
#include <miniOS/io.h>

/* thread_pool_lock is defined in sched.c; non-static so sched_balance.c can use it. */
extern spinlock_t thread_pool_lock;

/**
 * sched_balance_enqueue() - Enqueue @t on the CPU with the shortest run queue.
 * @t: Thread to enqueue. Must be in THREAD_BLOCKED state; caller sets RUNNABLE.
 *
 * Scans g_cpus[0..smp_cpu_count-1] for minimum queue_depth and tail-inserts
 * @t there. If the target CPU differs from the calling CPU, sends a
 * scheduler-kick IPI (SCHEDULER_KICK_VECTOR) to wake it from hlt.
 *
 * Context: Called from sched_create_thread() with thread_pool_lock NOT held.
 */
void sched_balance_enqueue(struct thread *t)
{
    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);

    /* Find CPU with shortest queue (lowest queue_depth) */
    uint32_t best_cpu  = 0;
    uint32_t min_depth = g_cpus[0].queue_depth;
    for (uint32_t i = 1; i < smp_cpu_count; i++) {
        if (g_cpus[i].queue_depth < min_depth) {
            min_depth = g_cpus[i].queue_depth;
            best_cpu  = i;
        }
    }

    /* Tail-insert t into best_cpu's circular queue */
    cpu_t *target                 = &g_cpus[best_cpu];
    t->next                       = target->run_queue_tail->next;  /* = run_queue_head */
    target->run_queue_tail->next  = t;
    target->run_queue_tail        = t;
    target->queue_depth++;

    spinlock_irqrestore(&thread_pool_lock, flags);

    /* Wake the target CPU from hlt if it differs from the current CPU.
     * Interrupts are re-enabled at this point (after irqrestore), so the IPI
     * is safe to send without holding the spinlock. */
    if (best_cpu != cpu_local()->cpu_id) {
        lapic_send_ipi(g_cpus[best_cpu].lapic_id, SCHEDULER_KICK_VECTOR);
    }
}

/**
 * sched_work_steal() - Steal half the work from the busiest CPU.
 *
 * Called from idle_loop() when the current CPU's run queue is empty.
 * Acquires thread_pool_lock, finds the CPU with the highest queue_depth > 1,
 * steals floor(queue_depth/2) THREAD_RUNNABLE threads, transfers them to the
 * calling CPU's queue.
 *
 * Context: Called with interrupts enabled (from idle_loop before hlt).
 * Does nothing if no stealable work exists.
 */
void sched_work_steal(void)
{
    cpu_t *local_cpu = cpu_local();

    /* Only steal if our queue is empty (queue_depth == 0 means only idle is present) */
    if (local_cpu->queue_depth > 0)
        return;

    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);

    /* Find busiest CPU with stealable work (queue_depth > 1 means at least 1 non-idle thread) */
    uint32_t busiest_cpu = MAX_CPUS;   /* sentinel: no victim found yet */
    uint32_t max_depth   = 1;          /* steal only if victim has > 1 runnable thread */

    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (i == local_cpu->cpu_id)
            continue;   /* skip ourselves */
        if (g_cpus[i].queue_depth > max_depth) {
            max_depth   = g_cpus[i].queue_depth;
            busiest_cpu = i;
        }
    }

    if (busiest_cpu == MAX_CPUS) {
        /* No stealable work */
        spinlock_irqrestore(&thread_pool_lock, flags);
        return;
    }

    cpu_t         *victim      = &g_cpus[busiest_cpu];
    uint32_t       steal_count = victim->queue_depth / 2;

    /* Walk victim queue to find the split boundary.
     * victim->run_queue_head is the first thread after the idle sentinel.
     * We steal steal_count threads starting from run_queue_head.
     *
     * Before steal:  idle <-> T1 <-> T2 <-> T3 <-> T4 <-> (back to idle)
     *   steal_count=2: steal T1, T2; victim keeps T3, T4
     *
     * Pointer manipulation:
     *   steal_head = victim->run_queue_head
     *   walk steal_count-1 steps to find steal_tail
     *   new_victim_head = steal_tail->next
     *   victim->run_queue_head = new_victim_head
     *   victim->run_queue_tail->next = new_victim_head  (close victim's circle)
     *   IF all stolen (steal_tail->next == victim->run_queue_head before update):
     *       victim queue is now idle-only; victim->run_queue_head = idle; tail = idle
     *
     * Append stolen chain to local queue:
     *   steal_tail->next = local->run_queue_tail->next  (= local->run_queue_head)
     *   local->run_queue_tail->next = steal_head
     *   local->run_queue_tail = steal_tail
     */

    struct thread *steal_head = victim->run_queue_head;
    struct thread *steal_tail = steal_head;

    /* Walk steal_count-1 steps to reach steal_tail */
    for (uint32_t i = 1; i < steal_count; i++) {
        steal_tail = steal_tail->next;
    }

    /* new head of victim's queue is the node after steal_tail */
    struct thread *new_victim_head = steal_tail->next;

    /* Detach stolen chain from victim */
    if (new_victim_head == victim->run_queue_head) {
        /* Stole everything: victim queue becomes idle-only sentinel */
        victim->run_queue_head         = victim->idle_thread;
        victim->run_queue_tail         = victim->idle_thread;
        victim->idle_thread->next      = victim->idle_thread;
    } else {
        victim->run_queue_head         = new_victim_head;
        victim->run_queue_tail->next   = new_victim_head;  /* close victim's circle */
    }
    victim->queue_depth -= steal_count;

    /* Attach stolen chain to local queue (tail-insert the whole chain) */
    steal_tail->next                 = local_cpu->run_queue_tail->next;  /* = local head */
    local_cpu->run_queue_tail->next  = steal_head;
    local_cpu->run_queue_tail        = steal_tail;
    local_cpu->queue_depth          += steal_count;

    spinlock_irqrestore(&thread_pool_lock, flags);

    printk("sched: CPU%u stole %u threads from CPU%u\n",
           local_cpu->cpu_id, steal_count, busiest_cpu);
}
