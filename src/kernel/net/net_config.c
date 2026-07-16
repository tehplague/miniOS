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

#include <miniOS/net/net_config.h>
#include <string.h>

static net_ipv4_config_t g_active_config;
static bool g_active_config_initialized;

void net_config_load_qemu_defaults(net_ipv4_config_t *config_out)
{
    static const uint8_t default_address[4] = { 10, 0, 2, 15 };
    static const uint8_t default_netmask[4] = { 255, 255, 255, 0 };
    static const uint8_t default_gateway[4] = { 10, 0, 2, 2 };
    static const uint8_t default_dns[4] = { 8, 8, 8, 8 };

    if (!config_out)
        return;

    memset(config_out, 0, sizeof(*config_out));
    memcpy(config_out->address, default_address, sizeof(default_address));
    memcpy(config_out->netmask, default_netmask, sizeof(default_netmask));
    memcpy(config_out->gateway, default_gateway, sizeof(default_gateway));
    memcpy(config_out->dns, default_dns, sizeof(default_dns));
    memcpy(config_out->if_name, "eth0", 5);
}

static void net_config_ensure_active(void)
{
    if (!g_active_config_initialized) {
        net_config_load_qemu_defaults(&g_active_config);
        g_active_config_initialized = true;
    }

    (void)net_config_apply_device_identity(&g_active_config);
}

const net_ipv4_config_t *net_config_get_active(void)
{
    net_config_ensure_active();
    return &g_active_config;
}

int net_config_apply_device_identity(net_ipv4_config_t *config)
{
    netdev_t *dev;

    if (!config)
        return -1;

    dev = netdev_first();
    if (!dev)
        return -1;

    memcpy(config->mac, dev->mac, sizeof(config->mac));
    memset(config->if_name, 0, sizeof(config->if_name));
    memcpy(config->if_name, dev->name, sizeof(config->if_name) - 1);
    return 0;
}

#ifdef TEST_BUILD
void net_config_set_active(const net_ipv4_config_t *config)
{
    if (!config)
        return;

    memcpy(&g_active_config, config, sizeof(g_active_config));
    g_active_config_initialized = true;
}
#endif
