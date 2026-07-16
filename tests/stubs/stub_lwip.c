// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// stub_lwip.c — minimal lwIP function stubs for host-native unit tests.
// Only the symbols referenced by net_socket.c's net_sock_send path need
// to be present; the test exercises alloc/free/recv, not send.

#include <stdint.h>
#include <stddef.h>

#ifndef u8_t
typedef uint8_t  u8_t;
#endif
#ifndef u16_t
typedef uint16_t u16_t;
#endif
#ifndef u32_t
typedef uint32_t u32_t;
#endif
#ifndef err_t
typedef int8_t   err_t;
#endif

#ifndef ERR_OK
#define ERR_OK   ((err_t)0)
#endif
#ifndef ERR_MEM
#define ERR_MEM  ((err_t)-1)
#endif

// Minimal pbuf stub
struct pbuf {
    struct pbuf *next;
    void        *payload;
    u16_t        tot_len;
    u16_t        len;
    u8_t         type_internal;
    u8_t         flags;
    u16_t        ref;
};

// pbuf_alloc: always return NULL in test build (net_sock_send returns -1)
struct pbuf *pbuf_alloc(int layer, u16_t length, int type) {
    (void)layer; (void)length; (void)type;
    return NULL;
}
void pbuf_free(struct pbuf *p) { (void)p; }
u16_t pbuf_copy_partial(const struct pbuf *buf, void *dataptr, u16_t len, u16_t offset) {
    (void)buf; (void)dataptr; (void)len; (void)offset; return 0;
}

// UDP stubs
struct udp_pcb;
err_t udp_send(struct udp_pcb *pcb, struct pbuf *p) { (void)pcb; (void)p; return ERR_MEM; }
err_t udp_sendto(struct udp_pcb *pcb, struct pbuf *p, const void *dst_ip, u16_t dst_port) {
    (void)pcb; (void)p; (void)dst_ip; (void)dst_port; return ERR_MEM;
}

// TCP stubs
struct tcp_pcb;
err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags) {
    (void)pcb; (void)dataptr; (void)len; (void)apiflags; return ERR_MEM;
}
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; return ERR_MEM; }

// TCP stubs (full set — syscall.c uses all paths)
void  tcp_recved(struct tcp_pcb *pcb, u16_t len)  { (void)pcb; (void)len; }
struct tcp_pcb *tcp_new(void)                      { return NULL; }
err_t tcp_bind(struct tcp_pcb *pcb, const void *ip, u16_t port) {
    (void)pcb; (void)ip; (void)port; return ERR_MEM; }
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *pcb, u8_t backlog) {
    (void)pcb; (void)backlog; return NULL; }
struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb)    { (void)pcb; return NULL; }
err_t tcp_connect(struct tcp_pcb *pcb, const void *ip, u16_t port, void *cb) {
    (void)pcb; (void)ip; (void)port; (void)cb; return ERR_MEM; }
void  tcp_accept(struct tcp_pcb *pcb, void *cb)    { (void)pcb; (void)cb; }
void  tcp_recv(struct tcp_pcb *pcb, void *cb)      { (void)pcb; (void)cb; }
void  tcp_arg(struct tcp_pcb *pcb, void *arg)      { (void)pcb; (void)arg; }
void  tcp_abort(struct tcp_pcb *pcb)               { (void)pcb; }

// UDP stubs (full set)
struct udp_pcb *udp_new(void)                      { return NULL; }
err_t udp_bind(struct udp_pcb *pcb, const void *ip, u16_t port) {
    (void)pcb; (void)ip; (void)port; return ERR_MEM; }
err_t udp_connect(struct udp_pcb *pcb, const void *ip, u16_t port) {
    (void)pcb; (void)ip; (void)port; return ERR_MEM; }
void  udp_recv(struct udp_pcb *pcb, void *cb, void *arg) { (void)pcb; (void)cb; (void)arg; }
void  udp_remove(struct udp_pcb *pcb)              { (void)pcb; }

// Raw stubs (SOCK_RAW path added in phase 43)
struct raw_pcb;
err_t raw_send(struct raw_pcb *pcb, struct pbuf *p) { (void)pcb; (void)p; return ERR_MEM; }
err_t raw_sendto(struct raw_pcb *pcb, struct pbuf *p, const void *ipaddr) {
    (void)pcb; (void)p; (void)ipaddr; return ERR_MEM; }
struct raw_pcb *raw_new(u8_t proto)                { (void)proto; return NULL; }
void  raw_recv(struct raw_pcb *pcb, void *cb, void *arg) { (void)pcb; (void)cb; (void)arg; }
void  raw_remove(struct raw_pcb *pcb)              { (void)pcb; }

// lwip_htons: byte-swap on little-endian host
u16_t lwip_htons(u16_t x) { return (u16_t)((x >> 8) | (x << 8)); }

/* -----------------------------------------------------------------------
 * net_observe_rx stub — referenced by lwip_port.c in test_lwip_port.
 * --------------------------------------------------------------------- */
int net_observe_rx(void *dev, const void *frame, size_t frame_len) {
    (void)dev; (void)frame; (void)frame_len;
    return 0;
}

/* -----------------------------------------------------------------------
 * TSC time-source stubs — referenced by syscall_mm.inc clock_gettime path.
 * Provide dummy values so the test binary links without real TSC hardware.
 * --------------------------------------------------------------------- */
uint64_t miniOS_tsc_boot = 0;
uint64_t miniOS_tsc_hz   = 0;   /* 0 → clock_gettime falls back to zero sec */

/* -----------------------------------------------------------------------
 * lwIP ip_data + dns_gethostbyname stubs — referenced by syscall_net.inc.
 * --------------------------------------------------------------------- */
struct ip_globals { int dummy; };
struct ip_globals ip_data;

#ifndef ERR_INPROGRESS
#define ERR_INPROGRESS ((err_t)-5)
#endif
err_t dns_gethostbyname(const char *hostname, void *addr, void *found, void *callback_arg) {
    (void)hostname; (void)addr; (void)found; (void)callback_arg;
    return ERR_INPROGRESS;
}
