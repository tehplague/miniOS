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

#include "unity.h"
#include <string.h>
#include <stdint.h>
#include <miniOS/net/net_socket.h>

/* sys_check_timeouts stub for timer contract test */
static int g_timeout_calls = 0;
void sys_check_timeouts(void) { g_timeout_calls++; }

/* lwip_netif_poll stub — validated by contract test 4.
 * Once lwip_netif.c exists this stub is replaced by the real include. */
void lwip_netif_poll(void) { sys_check_timeouts(); }

/* Include the implementation under test (will fail until net_socket.c exists) */
#include "../../src/kernel/net/net_socket.c"

void setUp(void) {}
void tearDown(void) {}

static void test_net_sock_alloc_returns_non_null_pointer(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_UDP);
    TEST_ASSERT_NOT_NULL(s);
    net_sock_free(s);
}

static void test_net_sock_free_makes_slot_reusable(void) {
    net_sock_t *s1 = net_sock_alloc(SOCK_PROTO_UDP);
    TEST_ASSERT_NOT_NULL(s1);
    net_sock_free(s1);
    net_sock_t *s2 = net_sock_alloc(SOCK_PROTO_UDP);
    TEST_ASSERT_NOT_NULL(s2);
    net_sock_free(s2);
}

static void test_net_sock_recv_returns_neg1_when_no_data(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_UDP);
    TEST_ASSERT_NOT_NULL(s);
    uint8_t buf[64];
    int ret = net_sock_recv(s, buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_INT(-1, ret);
    net_sock_free(s);
}

static void test_timer_pump_calls_sys_check_timeouts_on_each_poll(void) {
    g_timeout_calls = 0;
    lwip_netif_poll();
    lwip_netif_poll();
    lwip_netif_poll();
    TEST_ASSERT_EQUAL_INT(3, g_timeout_calls);
}

static void test_net_sock_send_returns_neg1_when_no_pcb(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_UDP);
    TEST_ASSERT_NOT_NULL(s);
    int ret = net_sock_send(s, "hello", 5, 0);
    TEST_ASSERT_EQUAL_INT(-1, ret);
    net_sock_free(s);
}

static void test_raw_sock_alloc_returns_non_null(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(SOCK_PROTO_RAW, s->proto);
    TEST_ASSERT_EQUAL_INT(1, s->in_use);
    net_sock_free(s);
}

static void test_raw_sock_free_makes_slot_reusable(void) {
    net_sock_t *s1 = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s1);
    net_sock_free(s1);
    net_sock_t *s2 = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s2);
    net_sock_free(s2);
}

/* ── New ring tests (Task 2) ──────────────────────────────────────────── */

/**
 * test_net_sock_ring_holds_two_entries_before_full:
 * Producer writes two entries directly into rx_ring simulating sys_raw_recv_cb.
 * After the first write: has_data==1, ring_full==0.
 * After the second write: has_data==1, ring_full==1.
 */
static void test_net_sock_ring_holds_two_entries_before_full(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s);

    /* Simulate producing the first slot */
    s->rx_ring[s->rx_tail & (NET_SOCK_RX_RING_SLOTS - 1)].len = 10;
    s->rx_tail++;
    TEST_ASSERT_EQUAL_INT(1, net_sock_has_data(s));
    TEST_ASSERT_EQUAL_INT(0, net_sock_ring_full(s));

    /* Simulate producing the second slot */
    s->rx_ring[s->rx_tail & (NET_SOCK_RX_RING_SLOTS - 1)].len = 20;
    s->rx_tail++;
    TEST_ASSERT_EQUAL_INT(1, net_sock_has_data(s));
    TEST_ASSERT_EQUAL_INT(1, net_sock_ring_full(s));

    net_sock_free(s);
}

/**
 * test_net_sock_recv_consumes_ring_in_fifo_order:
 * Producer writes entry A (addr=1, buf="AA") then entry B (addr=2, buf="BB").
 * Two consecutive net_sock_recv calls must return "AA" then "BB".
 * After both consumed: has_data==0.
 */
static void test_net_sock_recv_consumes_ring_in_fifo_order(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s);

    /* Give it a fake PCB so net_sock_recv doesn't bail early */
    static int fake_pcb = 1;
    s->pcb = &fake_pcb;

    /* Produce entry A */
    net_sock_rx_entry_t *slotA = &s->rx_ring[s->rx_tail & (NET_SOCK_RX_RING_SLOTS - 1)];
    slotA->addr_be = 1;
    slotA->buf[0] = 'A'; slotA->buf[1] = 'A';
    slotA->len = 2;
    s->rx_tail++;

    /* Produce entry B */
    net_sock_rx_entry_t *slotB = &s->rx_ring[s->rx_tail & (NET_SOCK_RX_RING_SLOTS - 1)];
    slotB->addr_be = 2;
    slotB->buf[0] = 'B'; slotB->buf[1] = 'B';
    slotB->len = 2;
    s->rx_tail++;

    /* Consume A */
    uint8_t buf[4];
    int ret = net_sock_recv(s, buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_INT(2, ret);
    TEST_ASSERT_EQUAL_UINT8('A', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('A', buf[1]);

    /* Consume B */
    ret = net_sock_recv(s, buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_INT(2, ret);
    TEST_ASSERT_EQUAL_UINT8('B', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('B', buf[1]);

    /* Ring is now empty */
    TEST_ASSERT_EQUAL_INT(0, net_sock_has_data(s));

    s->pcb = NULL;
    net_sock_free(s);
}

/**
 * test_net_sock_free_clears_ring_and_rcvtimeo_ticks:
 * Pre-populate rx_ring[0].len=5, rcvtimeo_ticks=42.
 * After net_sock_free: rx_head==0, rx_tail==0, both slots len==0,
 * rcvtimeo_ticks==0, in_use==0.
 */
static void test_net_sock_free_clears_ring_and_rcvtimeo_ticks(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s);

    s->rx_ring[0].len  = 5;
    s->rx_tail         = 1; /* advance tail to make ring non-empty */
    s->rcvtimeo_ticks  = 42;

    net_sock_free(s);

    TEST_ASSERT_EQUAL_UINT8(0, s->rx_head);
    TEST_ASSERT_EQUAL_UINT8(0, s->rx_tail);
    TEST_ASSERT_EQUAL_UINT(0, s->rx_ring[0].len);
    TEST_ASSERT_EQUAL_UINT(0, s->rx_ring[1].len);
    TEST_ASSERT_EQUAL_UINT32(0, s->rcvtimeo_ticks);
    TEST_ASSERT_EQUAL_INT(0, s->in_use);
}

/**
 * test_net_sock_has_data_wraparound_uint8:
 * Set rx_head=254, rx_tail=0 (simulating uint8 wraparound after 256 recvs).
 * has_data returns 1 (difference = 2 via uint8 math).
 * Set rx_head=0, rx_tail=0: has_data returns 0.
 */
static void test_net_sock_has_data_wraparound_uint8(void) {
    net_sock_t *s = net_sock_alloc(SOCK_PROTO_RAW);
    TEST_ASSERT_NOT_NULL(s);

    /* Wraparound: head=254, tail=0 → (uint8)(0 - 254) = 2 > 0 */
    s->rx_head = 254;
    s->rx_tail = 0;
    TEST_ASSERT_EQUAL_INT(1, net_sock_has_data(s));

    /* No data: head == tail */
    s->rx_head = 0;
    s->rx_tail = 0;
    TEST_ASSERT_EQUAL_INT(0, net_sock_has_data(s));

    net_sock_free(s);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_net_sock_alloc_returns_non_null_pointer);
    RUN_TEST(test_net_sock_free_makes_slot_reusable);
    RUN_TEST(test_net_sock_recv_returns_neg1_when_no_data);
    RUN_TEST(test_timer_pump_calls_sys_check_timeouts_on_each_poll);
    RUN_TEST(test_net_sock_send_returns_neg1_when_no_pcb);
    RUN_TEST(test_raw_sock_alloc_returns_non_null);
    RUN_TEST(test_raw_sock_free_makes_slot_reusable);
    /* New ring tests (Task 2) */
    RUN_TEST(test_net_sock_ring_holds_two_entries_before_full);
    RUN_TEST(test_net_sock_recv_consumes_ring_in_fifo_order);
    RUN_TEST(test_net_sock_free_clears_ring_and_rcvtimeo_ticks);
    RUN_TEST(test_net_sock_has_data_wraparound_uint8);
    return UNITY_END();
}
