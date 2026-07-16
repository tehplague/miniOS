#ifndef _MINIOS_NET_UNIX_SOCK_H_
#define _MINIOS_NET_UNIX_SOCK_H_

#include <miniOS/types.h>
#include <miniOS/arch/x86_64/spinlock.h>

#define UNIX_SOCK_POOL_SIZE 8
#define UNIX_SOCK_BUF_SIZE  4096
#define UNIX_SOCK_PATH_MAX  108
#define UNIX_BACKLOG_MAX    4

typedef enum {
    UNIX_STATE_FREE = 0,
    UNIX_STATE_BOUND,
    UNIX_STATE_LISTENING,
    UNIX_STATE_CONNECTED,
    UNIX_STATE_CONNECTING,  /* queued in server backlog; peer not yet assigned */
} unix_state_t;

typedef struct unix_sock {
    int          in_use;
    int          ref_count;  /* fd refs + backlog refs; freed when 0 */
    unix_state_t state;
    char         path[UNIX_SOCK_PATH_MAX];
    struct unix_sock *peer;               /* connected peer; NULL if not connected */
    struct unix_sock *backlog[UNIX_BACKLOG_MAX];
    int              backlog_head, backlog_tail;
    uint8_t  buf[UNIX_SOCK_BUF_SIZE];
    uint32_t buf_head, buf_tail;
    spinlock_t lock;
} unix_sock_t;

unix_sock_t *unix_sock_alloc(void);
void         unix_sock_free(unix_sock_t *s);
int          unix_sock_bind(unix_sock_t *s, const char *path);
int          unix_sock_listen(unix_sock_t *s, int backlog);
unix_sock_t *unix_sock_accept(unix_sock_t *server);  /* blocks; returns new peer socket */
int          unix_sock_connect(unix_sock_t *s, const char *path);  /* blocks until accepted */
int          unix_sock_write(unix_sock_t *s, const void *buf, size_t len);
int          unix_sock_read(unix_sock_t *s, void *buf, size_t len);   /* blocks if empty */
int          unix_sock_socketpair(unix_sock_t **a, unix_sock_t **b);
int          unix_sock_close(unix_sock_t *s);
int          unix_sock_has_data(unix_sock_t *s);

#endif /* _MINIOS_NET_UNIX_SOCK_H_ */
