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

#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/io.h>
#include <miniOS/sched/sched.h>
#ifdef CONSOLE_GOP
#include <miniOS/drivers/vt.h>
#endif

/* Calibration comment preserved for reference:
   PIT channel 2 OUT bit polling was removed because port 0x61 bit 5 does
   not toggle reliably under QEMU/KVM.  Calibration now uses a 10ms busy-loop
   via outb(0x80) × 10000 (each ≈ 1 µs, no KVM PAUSE_FILTER overhead). */

volatile uint32_t *lapic_base = NULL;

/* BSP-calibrated LAPIC timer ICR value.
   Written by lapic_timer_init() so APs can reuse the same value
   without running the PIT calibration again (PIT is not SMP-safe). */
uint32_t lapic_bsp_timer_icr = 0;

static void pic_disable(void) {
    /* Mask all IRQs on both master and slave 8259 PICs.
       Must be done BEFORE enabling LAPIC to prevent stray PIC interrupts. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void lapic_init(void) {
    /* Step 1: mask/silence the 8259 PIC */
    pic_disable();

    /* Step 2: read IA32_APIC_BASE MSR to get LAPIC physical base address.
       Bits 12..35 hold the base (page-aligned). On QEMU default = 0xFEE00000.
       The physical address 0xFEE00000 is within the identity-mapped 0..1GB range.
       Use explicit lo/hi split — more portable with freestanding gcc -mcmodel=large. */
    uint32_t msr_lo, msr_hi;
    __asm__ volatile(
        "rdmsr"
        : "=a"(msr_lo), "=d"(msr_hi)
        : "c"(IA32_APIC_BASE_MSR)
    );
    uint64_t msr_val = ((uint64_t)msr_hi << 32) | msr_lo;
    uint64_t base_phys = msr_val & 0xFFFFF000ULL;
    lapic_base = (volatile uint32_t *)(base_phys + KERNEL_VMA);

    /* Step 3: set global enable bit in MSR (bit 11), write back */
    msr_val |= IA32_APIC_BASE_ENABLE;
    __asm__ volatile(
        "wrmsr"
        :
        : "c"(IA32_APIC_BASE_MSR),
          "a"((uint32_t)msr_val),
          "d"((uint32_t)(msr_val >> 32))
    );

    /* Step 4: install spurious interrupt vector handler in IDT so the CPU
       has somewhere to go if a spurious interrupt occurs.
       Vector 0xFF, DPL 0, interrupt gate. The IDT must already have been
       loaded (idt_init + exceptions_init called before us).
       Uses a minimal stub that just iretqs — no EOI needed for spurious vectors. */
    extern void lapic_spurious_isr(void);
    idt_set_handler(LAPIC_SPURIOUS_VECTOR, (addr_t)lapic_spurious_isr,
                    KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);

    /* Register scheduler-kick IPI handler (used by sched_balance_enqueue to wake
       remote CPUs from hlt when a task arrives on their run queue). */
    extern void sched_kick_wrapper(void);
    idt_set_handler(SCHEDULER_KICK_VECTOR, (addr_t)sched_kick_wrapper,
                    KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);

    /* Register TLB-shootdown IPI handler (used by ipi_tlb_shootdown to flush
       remote CPU TLB entries after sys_munmap unmaps a page). */
    extern void tlb_shootdown_wrapper(void);
    idt_set_handler(TLB_SHOOTDOWN_VECTOR, (addr_t)tlb_shootdown_wrapper,
                    KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);

    /* Register panic-halt IPI handler (used by ipi_panic_halt to stop all APs
       when a kernel exception occurs on any CPU). */
    extern void panic_halt_wrapper(void);
    idt_set_handler(PANIC_HALT_VECTOR, (addr_t)panic_halt_wrapper,
                    KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);

    /* Step 5: software-enable LAPIC via SVR register.
       Write spurious vector number | LAPIC_SVR_ENABLE. */
    lapic_write(LAPIC_SVR, LAPIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);

    printk("LAPIC: base=0x%llx id=%u enabled\n",
           (uint64_t)base_phys, lapic_read(LAPIC_ID) >> 24);
}

/* Non-inline EOI wrapper callable from assembly (irq_wrapper stubs in isr.asm). */
void lapic_eoi_asm(void) {
    lapic_eoi();
}

/* ------------------------------------------------------------------ */
/*  LAPIC timer                                                        */
/* ------------------------------------------------------------------ */

volatile uint64_t lapic_tick_count = 0;

/* TSC frequency in Hz, set by lapic_timer_init().
   Non-zero after init; clock_gettime/gettimeofday use this for sub-microsecond
   resolution.  Never zero: a 2 GHz conservative fallback is applied if PIT
   calibration fails (D-09, D-11). */
uint64_t miniOS_tsc_hz   = 0;
uint64_t miniOS_tsc_boot = 0;

static inline uint64_t lapic_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Timer ISR called from lapic_timer_wrapper asm stub.
   EOI is sent here so the LAPIC can accept the next timer interrupt. */
void lapic_timer_isr(void *frame) {
    (void)frame;
    lapic_tick_count++;
    lapic_eoi_asm();   /* EOI BEFORE sched_tick: allows new timer interrupts while we context switch */
    sched_tick();      /* may switch to a different thread — must come after EOI */
#ifdef CONSOLE_GOP
    if (cpu_local()->cpu_id == 0)
        vt_cursor_tick();
#endif
}

/* lapic_timer_idt_install — install the IDT entry for LAPIC_TIMER_VECTOR (48).
   Called from main.c BEFORE smp_boot_aps() so that APs' LAPIC timers can fire
   safely once they start.  lapic_timer_init() previously installed this entry,
   but that function is called after smp_boot_aps() — too late: the APs already
   have running timers by then and would #GP on vector 48, triggering
   ipi_panic_halt() which halts the BSP. */
void lapic_timer_idt_install(void) {
    extern addr_t lapic_timer_wrapper;
    idt_set_handler(LAPIC_TIMER_VECTOR, (addr_t)&lapic_timer_wrapper,
                    KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);
}

void lapic_timer_init(void) {
    /* Calibrate LAPIC timer ticks per 10ms using a port-0x80 busy-loop.
       IDT entry for LAPIC_TIMER_VECTOR is already installed by
       lapic_timer_idt_install() (called before smp_boot_aps()).

       Divide-by-16 (DCR=0x3): on QEMU TCG the bus clock is ~1 GHz,
       so the effective rate is ~62.5 MHz, giving ~625 000 ticks per 10ms. */

    /* Step 1: set LAPIC timer divisor, mask LVT, load max initial count */
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIVBY_16);
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Step 3: wait 10ms using a plain decrement loop.
       'outb(0x80)' was replaced because when APs' LAPIC timers are running
       their VM exits contend with BSP's outb exits, making the loop take
       much longer than intended.  A plain decrement runs entirely in-guest.
       10 million iterations at ~1 GHz KVM effective rate ≈ 10ms.

       We also use this loop to calibrate the TSC frequency: read TSC before
       and after the 10ms busy-loop, then scale to Hz.  This piggybacks on the
       LAPIC calibration delay so we only spin once. */
    uint64_t tsc_start = lapic_rdtsc();
    for (volatile uint32_t i = 10000000U; i > 0; i--) {
        __asm__ volatile("" ::: "memory");
    }
    uint64_t tsc_end = lapic_rdtsc();

    /* Step 4: read LAPIC current count and compute ticks per 10ms */
    uint32_t lapic_ccr = lapic_read(LAPIC_TIMER_CCR);
    uint32_t ticks_per_10ms = 0xFFFFFFFF - lapic_ccr;

    /* Step 5: ticks_per_10ms IS the ICR for 100 Hz (one tick every 10ms) */
    /* WR-05: sanity-floor for suspiciously low calibration results (e.g. the
     * busy-loop ran faster than expected and the LAPIC count barely moved).
     * 10 000 ticks at div/16 corresponds to <1ms of actual measurement — too
     * short to be reliable.  Fall back to 625 000 (62.5 MHz / 100 Hz). */
    if (ticks_per_10ms < 10000)
        ticks_per_10ms = 625000;
    uint32_t icr = ticks_per_10ms;
    if (icr == 0) icr = 1000000; /* fallback if measurement unavailable */

    /* Step 6: save calibrated ICR for APs (they reuse this value) */
    lapic_bsp_timer_icr = icr;

    /* Step 7: TSC calibration — compute Hz from the 10ms busy-loop window.
       tsc_end - tsc_start is the number of TSC ticks in ~10ms; multiply by 100
       to get Hz.  If the measurement is invalid, use a 2 GHz conservative
       estimate (D-09: miniOS_tsc_hz must never be zero). */
    /* PIT ch2 TSC calibration: 1,193,182 Hz crystal → 11,932 ticks ≈ 10 ms.
       Accurate regardless of QEMU/KVM mode or CPU speed, unlike the busy-loop. */
    int tsc_fallback = 0;
    {
        uint8_t pit_save = inb(0x61);
        outb(0x61, (pit_save & ~0x02) | 0x01);  /* gate2=1, speaker off */
        outb(0x43, 0xb0);                        /* ch2, lo/hi, mode 0, binary */
        outb(0x42, 0x9c);                        /* count=11932 low byte */
        outb(0x42, 0x2e);                        /* count=11932 high byte → starts */
        uint64_t pit_tsc0 = lapic_rdtsc();
        while ((inb(0x61) & 0x20) == 0) {}      /* spin until OUT2 high (count expired) */
        uint64_t pit_tsc1 = lapic_rdtsc();
        outb(0x61, pit_save);

        if (pit_tsc1 > pit_tsc0) {
            miniOS_tsc_boot = pit_tsc0;
            miniOS_tsc_hz   = (pit_tsc1 - pit_tsc0) * 100ULL;
        } else {
            miniOS_tsc_boot = tsc_start;
            miniOS_tsc_hz   = 2000000000ULL;
            tsc_fallback    = 1;
        }
    }
    if (miniOS_tsc_hz == 0) {
        miniOS_tsc_hz = 2000000000ULL;
        tsc_fallback  = 1;
    }

    /* Step 8: start periodic timer */
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIVBY_16);
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_ICR, icr);

    printk("LAPIC timer: %u ticks/10ms, ICR=%u (~100 Hz), TSC=%llu Hz%s\n",
           ticks_per_10ms, icr, miniOS_tsc_hz,
           tsc_fallback ? " [FALLBACK ESTIMATE]" : "");
}

/* lapic_timer_init_ap() — start LAPIC timer on the calling AP using
   the BSP's calibrated ICR (lapic_bsp_timer_icr). Must be called after
   lapic_timer_init() has run on the BSP. The IDT entry for vector 48 is
   already installed by lapic_timer_init() (IDT is shared); no re-install needed. */
void lapic_timer_init_ap(void) {
    uint32_t icr = lapic_bsp_timer_icr;
    if (icr == 0) icr = 1000000;  /* fallback if BSP hasn't calibrated yet */

    /* Start periodic timer on this AP using the same divisor and vector as BSP */
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIVBY_16);
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_ICR, icr);

    printk("AP CPU %u: LAPIC timer started, ICR=%u (~100 Hz)\n",
           cpu_local()->cpu_id, icr);
}

/* ------------------------------------------------------------------ */
/*  Scheduler-kick IPI (SCHEDULER_KICK_VECTOR = 50)                   */
/* ------------------------------------------------------------------ */

/**
 * lapic_send_ipi() - Send a Fixed IPI to target_lapic_id at vector.
 *
 * ICR encoding:
 *   ICR_HIGH[31:24] = target LAPIC ID
 *   ICR_LOW[7:0]    = vector
 *   ICR_LOW[10:8]   = 0b000 (Fixed delivery mode)
 *   ICR_LOW[14]     = 1 (Assert level)
 *   Writing ICR_LOW triggers the IPI.
 */
void lapic_send_ipi(uint8_t target_lapic_id, uint8_t vector)
{
    /* Write destination first; ICR_HIGH must be set before ICR_LOW. */
    lapic_write(LAPIC_ICR_HIGH, ((uint32_t)target_lapic_id) << 24);

    /* Fixed delivery mode (bits[10:8]=0b000), assert level (bit 14=1), vector */
    uint32_t icr_low = ((uint32_t)vector & 0xFF) | (1U << 14);
    lapic_write(LAPIC_ICR_LOW, icr_low);
}

/**
 * sched_kick_isr() - Scheduler-kick IPI C handler.
 * @frame: Interrupt frame pointer (unused).
 *
 * Called from sched_kick_wrapper on the remote CPU.
 * Sends EOI before calling sched_tick() so the LAPIC can accept
 * new interrupts while the context switch is in progress.
 */
void sched_kick_isr(void *frame)
{
    (void)frame;
    lapic_eoi_asm();
    sched_tick();
}
