/* stub_net_socket.c — fake net_socket layer for test_syscall.
 * syscall.c calls net_sock_alloc/free/recv/send/close; these stubs satisfy
 * the linker without pulling in the real lwIP stack. */

#include <miniOS/net/net_socket.h>

net_sock_t *net_sock_alloc(int proto)                              { (void)proto; return NULL; }
void        net_sock_free(net_sock_t *sock)                        { (void)sock; }
int         net_sock_recv(net_sock_t *s, void *b, size_t l, int f) { (void)s;(void)b;(void)l;(void)f; return -1; }
int         net_sock_send(net_sock_t *s, const void *b, size_t l, int f) { (void)s;(void)b;(void)l;(void)f; return -1; }
int         net_sock_close(net_sock_t *sock)                       { (void)sock; return 0; }

#include <miniOS/net/unix_sock.h>
int unix_sock_read(unix_sock_t *s, void *buf, size_t len)              { (void)s;(void)buf;(void)len; return -1; }
int unix_sock_write(unix_sock_t *s, const void *buf, size_t len)       { (void)s;(void)buf;(void)len; return -1; }
int unix_sock_close(unix_sock_t *s)                                    { (void)s; return 0; }
