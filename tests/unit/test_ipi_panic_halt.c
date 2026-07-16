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

/* test_ipi_panic_halt.c — unit tests for IPI-02: panic halt IPI ICR encoding
 * and vector value.
 * Self-contained: includes ipi.h (inline functions) and apic.h; no ipi.c needed.
 * stub_types.h is force-included via -include flag; do NOT redefine stdint types. */

#include "unity.h"
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/ipi/ipi.h>
#include <string.h>

/* ── Fake LAPIC MMIO array ── */
#define FAKE_LAPIC_REGS 200
static volatile uint32_t fake_lapic[FAKE_LAPIC_REGS];

/* Inline lapic_send_ipi logic using fake_lapic instead of real LAPIC MMIO */
static void send_ipi_sim(uint8_t target_lapic_id, uint8_t vector)
{
    fake_lapic[LAPIC_ICR_HIGH / 4] = ((uint32_t)target_lapic_id) << 24;
    uint32_t icr_low = ((uint32_t)vector & 0xFF) | (1U << 14);
    fake_lapic[LAPIC_ICR_LOW / 4] = icr_low;
}

void setUp(void)    { memset((void *)fake_lapic, 0, sizeof(fake_lapic)); }
void tearDown(void) { }

/** PANIC_HALT_VECTOR must be 52. */
void test_panic_halt_vector_value(void)
{
    TEST_ASSERT_EQUAL_UINT32(52, PANIC_HALT_VECTOR);
}

/** ICR_HIGH[31:24] must equal target_lapic_id after send_ipi_sim(7, PANIC_HALT_VECTOR). */
void test_panic_halt_ipi_icr_encoding_dest(void)
{
    send_ipi_sim(7, PANIC_HALT_VECTOR);
    TEST_ASSERT_EQUAL_UINT32(7U, fake_lapic[LAPIC_ICR_HIGH / 4] >> 24);
}

/** ICR_LOW[7:0] must equal 52 (PANIC_HALT_VECTOR) after send_ipi_sim(2, PANIC_HALT_VECTOR). */
void test_panic_halt_ipi_icr_encoding_vector(void)
{
    send_ipi_sim(2, PANIC_HALT_VECTOR);
    TEST_ASSERT_EQUAL_UINT32(PANIC_HALT_VECTOR,
                             fake_lapic[LAPIC_ICR_LOW / 4] & 0xFF);
}

/** ICR_LOW bit 14 (level assert) must be 1. */
void test_panic_halt_ipi_level_assert(void)
{
    send_ipi_sim(1, PANIC_HALT_VECTOR);
    TEST_ASSERT_TRUE((fake_lapic[LAPIC_ICR_LOW / 4] >> 14) & 1U);
}

/** ICR_LOW bits[10:8] (delivery mode) must be 0 (Fixed). */
void test_panic_halt_ipi_fixed_delivery(void)
{
    send_ipi_sim(3, PANIC_HALT_VECTOR);
    uint32_t delivery_mode = (fake_lapic[LAPIC_ICR_LOW / 4] >> 8) & 0x7;
    TEST_ASSERT_EQUAL_UINT32(0, delivery_mode);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_panic_halt_vector_value);
    RUN_TEST(test_panic_halt_ipi_icr_encoding_dest);
    RUN_TEST(test_panic_halt_ipi_icr_encoding_vector);
    RUN_TEST(test_panic_halt_ipi_level_assert);
    RUN_TEST(test_panic_halt_ipi_fixed_delivery);
    return UNITY_END();
}
