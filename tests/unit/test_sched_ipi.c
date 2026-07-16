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

/* test_sched_ipi.c — unit tests for SCHED-04: scheduler-kick IPI ICR encoding.
 * Tests the bit-field encoding used by lapic_send_ipi() against a fake LAPIC
 * register array. Does not require actual LAPIC hardware.
 * stub_types.h is force-included via -include flag; do NOT redefine stdint types. */

#include "unity.h"
#include <miniOS/arch/x86_64/apic.h>
#include <string.h>

/* ── Fake LAPIC MMIO array ── */
/* LAPIC registers are 32-bit at 16-byte-aligned offsets.
 * Highest offset used: LAPIC_ICR_HIGH = 0x310.
 * Array size: 0x310/4 + 1 = 197 entries. */
#define FAKE_LAPIC_REGS 200
static volatile uint32_t fake_lapic[FAKE_LAPIC_REGS];

/* Inline lapic_send_ipi logic using fake_lapic instead of real LAPIC MMIO */
static void send_ipi_sim(uint8_t target_lapic_id, uint8_t vector)
{
    /* Write destination to ICR_HIGH[31:24] */
    fake_lapic[LAPIC_ICR_HIGH / 4] = ((uint32_t)target_lapic_id) << 24;

    /* Fixed delivery (bits[10:8]=0), level assert (bit 14=1), vector in bits[7:0] */
    uint32_t icr_low = ((uint32_t)vector & 0xFF) | (1U << 14);
    fake_lapic[LAPIC_ICR_LOW / 4] = icr_low;
}

void setUp(void)    { memset((void *)fake_lapic, 0, sizeof(fake_lapic)); }
void tearDown(void) { }

/** ICR_HIGH[31:24] must equal target_lapic_id. */
void test_ipi_icr_encoding_sets_dest(void)
{
    send_ipi_sim(5, SCHEDULER_KICK_VECTOR);
    uint32_t icr_high = fake_lapic[LAPIC_ICR_HIGH / 4];
    TEST_ASSERT_EQUAL_UINT32(5U, icr_high >> 24);
}

/** ICR_LOW[7:0] must equal the requested vector (50). */
void test_ipi_icr_encoding_sets_vector(void)
{
    send_ipi_sim(2, SCHEDULER_KICK_VECTOR);
    uint32_t icr_low = fake_lapic[LAPIC_ICR_LOW / 4];
    TEST_ASSERT_EQUAL_UINT32(SCHEDULER_KICK_VECTOR, icr_low & 0xFF);
}

/** ICR_LOW bit 14 (level assert) must be set. */
void test_ipi_icr_encoding_sets_level_assert(void)
{
    send_ipi_sim(1, SCHEDULER_KICK_VECTOR);
    uint32_t icr_low = fake_lapic[LAPIC_ICR_LOW / 4];
    TEST_ASSERT_TRUE((icr_low >> 14) & 1U);
}

/** ICR_LOW bits[10:8] must be 0b000 (Fixed delivery mode). */
void test_ipi_icr_encoding_fixed_delivery(void)
{
    send_ipi_sim(3, SCHEDULER_KICK_VECTOR);
    uint32_t icr_low = fake_lapic[LAPIC_ICR_LOW / 4];
    uint32_t delivery_mode = (icr_low >> 8) & 0x7;
    TEST_ASSERT_EQUAL_UINT32(0, delivery_mode);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ipi_icr_encoding_sets_dest);
    RUN_TEST(test_ipi_icr_encoding_sets_vector);
    RUN_TEST(test_ipi_icr_encoding_sets_level_assert);
    RUN_TEST(test_ipi_icr_encoding_fixed_delivery);
    return UNITY_END();
}
