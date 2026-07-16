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

#include <miniOS/net/lwip_port.h>
#include <miniOS/net/net_observe.h>
#include <string.h>

typedef struct {
    netdev_t *dev;
    lwip_port_rx_callback_t rx_callback;
    void *rx_ctx;
    lwip_port_timeout_callback_t timeout_callback;
    void *timeout_ctx;
    uint32_t timeout_accumulator_ms;
    uint32_t drops;
} lwip_port_state_t;

static lwip_port_state_t g_lwip_port_state;

static void lwip_port_receive(netdev_t *dev,
                              const void *frame,
                              size_t frame_len,
                              void *ctx)
{
    lwip_port_state_t *state = (lwip_port_state_t *)ctx;
    ethernet_frame_info_t info;
    lwip_port_frame_kind_t kind;

    if (!state || !dev || !frame)
        return;

    /* Keep sysfs-backed device counters live even after lwIP installs the RX hook. */
    net_observe_rx(dev, frame, frame_len);

    if (!ethernet_frame_info_parse(frame, frame_len, &info)) {
        state->drops++;
        return;
    }

    kind = lwip_port_classify(&info);
    if (kind == LWIP_PORT_FRAME_UNSUPPORTED || !state->rx_callback) {
        state->drops++;
        return;
    }

    state->rx_callback(dev, kind, frame, frame_len, &info, state->rx_ctx);
}

netdev_t *lwip_port_attach(lwip_port_rx_callback_t rx_callback,
                               void *rx_ctx,
                               lwip_port_timeout_callback_t timeout_callback,
                               void *timeout_ctx)
{
    netdev_t *dev;

    if (!rx_callback)
        return NULL;

    dev = netdev_first();
    if (!dev)
        return NULL;

    memset(&g_lwip_port_state, 0, sizeof(g_lwip_port_state));
    g_lwip_port_state.dev = dev;
    g_lwip_port_state.rx_callback = rx_callback;
    g_lwip_port_state.rx_ctx = rx_ctx;
    g_lwip_port_state.timeout_callback = timeout_callback;
    g_lwip_port_state.timeout_ctx = timeout_ctx;

    dev->rx_handler = lwip_port_receive;
    dev->rx_handler_ctx = &g_lwip_port_state;
    return dev;
}

void lwip_port_poll(void)
{
    netdev_poll_all();
}

void lwip_port_poll_timeouts(uint32_t elapsed_ms)
{
    g_lwip_port_state.timeout_accumulator_ms += elapsed_ms;
    if (g_lwip_port_state.timeout_callback)
        g_lwip_port_state.timeout_callback(elapsed_ms, g_lwip_port_state.timeout_ctx);
}

int lwip_port_tx(const void *frame, size_t frame_len)
{
    netdev_t *registered = netdev_first();
    netdev_t *dev = g_lwip_port_state.dev;

    if (!registered) {
        g_lwip_port_state.dev = NULL;
        return -1;
    }

    if (!dev)
        dev = registered;
    if (!dev || !dev->ops || !dev->ops->xmit)
        return -1;

    g_lwip_port_state.dev = dev;
    int ret = dev->ops->xmit(dev, frame, frame_len);
    if (ret == 0)
        dev->stats.tx_bytes += frame_len;
    return ret;
}

lwip_port_frame_kind_t lwip_port_classify(const ethernet_frame_info_t *info)
{
    if (!info)
        return LWIP_PORT_FRAME_UNSUPPORTED;

    if (info->ethertype == ETHERTYPE_ARP)
        return LWIP_PORT_FRAME_ARP;
    if (info->ethertype == ETHERTYPE_IPV4)
        return LWIP_PORT_FRAME_IPV4;
    return LWIP_PORT_FRAME_UNSUPPORTED;
}

/* sys_now() — required by lwIP's sys_check_timeouts() in NO_SYS=1 mode.
 * LAPIC timer fires at ~100 Hz (10ms per tick). */
extern volatile uint64_t lapic_tick_count;
uint32_t sys_now(void)
{
    return (uint32_t)(lapic_tick_count * 10);
}

unsigned int miniOS_lwip_rand(void)
{
    static uint32_t state = 0x6d2b79f5u;
    uint32_t tick = (uint32_t)lapic_tick_count;

    state ^= tick + 0x9e3779b9u + (state << 6) + (state >> 2);
    return state;
}

#ifdef TEST_BUILD
netdev_t *lwip_port_attached_device(void)
{
    return g_lwip_port_state.dev;
}

uint32_t lwip_port_timeout_accumulator(void)
{
    return g_lwip_port_state.timeout_accumulator_ms;
}

uint32_t lwip_port_drop_count(void)
{
    return g_lwip_port_state.drops;
}
#endif
