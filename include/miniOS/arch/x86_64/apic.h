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

/**
 * @file apic.h
 * @defgroup apic LAPIC / IOAPIC
 * @brief Local and I/O APIC initialisation, IPI dispatch, and LAPIC timer.
 *
 * Provides the public API for:
 * - Local APIC (LAPIC): software-enable, EOI, IPI send.
 * - I/O APIC (IOAPIC): redirection table setup, IRQ mask/unmask.
 * - LAPIC timer: BSP calibration and AP reuse.
 * @{
 */

#ifndef _MINIOS_ARCH_X86_64_APIC_H_
#define _MINIOS_ARCH_X86_64_APIC_H_

#include <miniOS/types.h>

/* ------------------------------------------------------------------ */
/*  LAPIC MMIO register offsets (byte offsets from lapic_base)         */
/* ------------------------------------------------------------------ */
#define LAPIC_ID          0x020  /* Local APIC ID register             */
#define LAPIC_EOI         0x0B0  /* End-of-Interrupt register          */
#define LAPIC_SVR         0x0F0  /* Spurious Interrupt Vector Register */
#define LAPIC_ICR_LOW     0x300  /* Interrupt Command Register low     */
#define LAPIC_ICR_HIGH    0x310  /* Interrupt Command Register high    */
#define LAPIC_TIMER_LVT   0x320  /* LVT Timer register                 */
#define LAPIC_TIMER_ICR   0x380  /* Timer Initial Count Register       */
#define LAPIC_TIMER_CCR   0x390  /* Timer Current Count Register       */
#define LAPIC_TIMER_DCR   0x3E0  /* Timer Divide Configuration Register*/

/* ------------------------------------------------------------------ */
/*  SVR flags                                                          */
/* ------------------------------------------------------------------ */
#define LAPIC_SVR_ENABLE       (1 << 8)  /* Software enable bit        */
#define LAPIC_SPURIOUS_VECTOR  0xFF      /* Spurious interrupt vector  */

/* ------------------------------------------------------------------ */
/*  IA32_APIC_BASE MSR                                                 */
/* ------------------------------------------------------------------ */
#define IA32_APIC_BASE_MSR    0x1B
#define IA32_APIC_BASE_ENABLE (1 << 11)  /* Global enable bit          */

/* ------------------------------------------------------------------ */
/*  LAPIC base pointer (set by lapic_init)                             */
/* ------------------------------------------------------------------ */
extern volatile uint32_t *lapic_base;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Detect, identity-map, and software-enable the local APIC.
 *
 * Reads the IA32_APIC_BASE MSR to obtain the LAPIC physical base address,
 * maps it into the kernel higher-half window via vmm_map_page(), writes the
 * Spurious Interrupt Vector Register to set vector 0xFF and set the
 * software-enable bit (LAPIC_SVR_ENABLE), and stores the mapped pointer in
 * lapic_base. Also registers lapic_send_ipi for use by the scheduler.
 *
 * Context: Called once by the BSP during early boot, before smp_boot_aps().
 */
void lapic_init(void);

/**
 * @brief Write a 32-bit value to a LAPIC MMIO register.
 * @param reg Register offset in bytes from lapic_base (e.g. LAPIC_EOI = 0x0B0).
 * @param val 32-bit value to write.
 */
static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

/**
 * @brief Read a 32-bit value from a LAPIC MMIO register.
 * @param reg Register offset in bytes from lapic_base (e.g. LAPIC_ID = 0x020).
 * @return 32-bit value at the register.
 */
static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

/**
 * @brief Signal End-of-Interrupt to the local APIC.
 *
 * Writes 0 to LAPIC_EOI (offset 0x0B0). Must be called at the end of every
 * LAPIC-delivered interrupt handler before returning.
 */
static inline void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/**
 * @brief Non-inline wrapper for lapic_eoi(), callable from assembly.
 *
 * Provided because inline functions cannot be called by name from NASM/GAS
 * assembly stubs (no symbol is emitted). Forwards to lapic_eoi().
 */
void lapic_eoi_asm(void);

/**
 * @brief Send a Fixed IPI to a remote CPU.
 * @param target_lapic_id Hardware LAPIC ID of the destination CPU (from g_cpus[i].lapic_id).
 * @param vector IDT vector the remote CPU should execute (e.g. SCHEDULER_KICK_VECTOR).
 *
 * Writes destination to ICR_HIGH[31:24], then writes the fixed-delivery,
 * level-assert ICR_LOW to trigger the IPI.  Writing ICR_LOW sends the interrupt.
 *
 * Context: Must be called with interrupts disabled or with spinlock protection
 *          to prevent ICR_HIGH/ICR_LOW write from being interleaved.
 */
void lapic_send_ipi(uint8_t target_lapic_id, uint8_t vector);

/* ------------------------------------------------------------------ */
/*  I/O APIC MMIO base (physical).                                    */
/*  Identity-mapped via page_table_l2_mmio L2[502] (2MB huge page).  */
/* ------------------------------------------------------------------ */
#define IOAPIC_BASE         0xFEC00000UL
/* I/O APIC indirect registers (byte offsets from IOAPIC_BASE) */
#define IOAPIC_REGSEL       0x00   /* register select */
#define IOAPIC_IOWIN        0x10   /* data window */
/* I/O APIC indirect register indices */
#define IOAPIC_ID           0x00
#define IOAPIC_VER          0x01
#define IOAPIC_REDTBL_LO(n) (0x10 + (n)*2)     /* low 32 bits of redirection entry n */
#define IOAPIC_REDTBL_HI(n) (0x10 + (n)*2 + 1) /* high 32 bits of redirection entry n */
/* Redirection entry bits */
#define IOAPIC_RTE_MASKED   (1 << 16)           /* set to mask (disable) this IRQ line */
#define IOAPIC_RTE_LEVEL    (1 << 15)           /* 0=edge, 1=level trigger */
#define IOAPIC_RTE_LOPOL    (1 << 13)           /* 0=active-high, 1=active-low polarity */
/* First IDT vector for external IRQs (vectors 0-31 are CPU exceptions) */
#define IRQ_VECTOR_BASE     32

/**
 * @brief Map and initialise the I/O APIC.
 *
 * Maps IOAPIC_BASE (0xFEC00000) into the kernel address space via the
 * identity MMIO huge page. Masks all 24 redirection table entries, then
 * unmasks the entries for PS/2 keyboard (IRQ1) and ATA primary (IRQ14)
 * with edge-triggered, active-high, fixed-delivery mode.
 */
void ioapic_init(void);

/**
 * @brief Mask (disable) an I/O APIC redirection entry.
 * @param irq IRQ line index (0-23) to mask. Sets IOAPIC_RTE_MASKED in the
 *            low 32 bits of the redirection table entry for @irq.
 */
void ioapic_mask_irq(uint8_t irq);

/**
 * @brief Unmask (enable) an I/O APIC redirection entry.
 * @param irq IRQ line index (0-23) to unmask. Clears IOAPIC_RTE_MASKED in the
 *            low 32 bits of the redirection table entry for @irq.
 */
void ioapic_unmask_irq(uint8_t irq);

/* ------------------------------------------------------------------ */
/*  LAPIC timer                                                        */
/* ------------------------------------------------------------------ */
#define LAPIC_TIMER_PERIODIC    (1 << 17)   /* LVT timer mode: periodic */
#define LAPIC_TIMER_ONESHOT     (0 << 17)   /* LVT timer mode: one-shot */
#define LAPIC_TIMER_MASKED      (1 << 16)   /* LVT: mask the timer      */
#define LAPIC_TIMER_VECTOR      48          /* IDT vector for LAPIC timer */
#define SCHEDULER_KICK_VECTOR   50          /* IDT vector: scheduler-kick IPI from remote CPU */
#define TLB_SHOOTDOWN_VECTOR    51          /* IDT vector: TLB-shootdown IPI broadcast */
#define PANIC_HALT_VECTOR       52          /* IDT vector: panic-halt IPI broadcast */
#define LAPIC_TIMER_DIVBY_16    0x3         /* DCR divide-by-16         */

/**
 * @brief Install the IDT entry for LAPIC_TIMER_VECTOR (48).
 *
 * Must be called before smp_boot_aps() so that APs find a valid timer handler
 * from the moment they enable their LAPIC timers. Installs a DPL-0 interrupt
 * gate at IDT[LAPIC_TIMER_VECTOR].
 */
void lapic_timer_idt_install(void);

/**
 * @brief Calibrate and start the BSP LAPIC timer.
 *
 * Uses the PIT (8254) as a reference to measure the LAPIC timer frequency,
 * computes an ICR value for a ~10 ms periodic tick, stores it in
 * lapic_bsp_timer_icr, configures the LVT timer in periodic mode at
 * LAPIC_TIMER_VECTOR (48), and starts counting. Called once on the BSP.
 */
void lapic_timer_init(void);
extern volatile uint64_t lapic_tick_count;

/* BSP-calibrated LAPIC timer ICR value (written by lapic_timer_init).
   APs read this to start their timers without running PIT calibration again. */
extern uint32_t lapic_bsp_timer_icr;

/**
 * @brief Start the LAPIC timer on the calling AP.
 *
 * Reads lapic_bsp_timer_icr (calibrated by the BSP) and writes it directly
 * to LAPIC_TIMER_ICR, avoiding a second PIT calibration which would cause
 * contention. Sets LVT timer to periodic mode at LAPIC_TIMER_VECTOR.
 * Called from ap_entry() on each AP after LAPIC software-enable.
 */
void lapic_timer_init_ap(void);

/** @} */

#endif /* _MINIOS_ARCH_X86_64_APIC_H_ */
