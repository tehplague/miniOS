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

#ifndef _MINIOS_NET_LWIP_NETIF_H_
#define _MINIOS_NET_LWIP_NETIF_H_

/**
 * @brief Initialize lwIP stack and register eth0 netif.
 *
 * Calls lwip_init(), netif_add() with QEMU static IPv4 config from
 * net_config_get_active(), wires the RX callback to ethernet_input(),
 * and calls lwip_port_attach() to connect the Ethernet seam.
 * Must be called after net_observe_init() and before any socket operations.
 */
void lwip_netif_init(void);

/**
 * @brief Poll the network interface and drive lwIP timers.
 *
 * Calls lwip_port_poll() then sys_check_timeouts(). Must be called
 * regularly (every ~10ms) to keep TCP retransmit and ARP aging alive.
 * Also calls lwip_port_poll_timeouts(10) to feed the timeout accumulator.
 */
void lwip_netif_poll(void);

/**
 * @brief Kernel thread function that drives lwIP polling.
 *
 * Runs in an infinite loop: calls lwip_netif_poll() then sched_yield().
 * Receive callbacks call sched_wake_tid() to unblock threads sleeping in poll/select.
 * Start via sched_create_thread(net_poll_thread_fn, NULL) after lwip_netif_init().
 */
void net_poll_thread_fn(void *arg);

#endif /* _MINIOS_NET_LWIP_NETIF_H_ */
