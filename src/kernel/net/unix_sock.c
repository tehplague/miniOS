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

#include <miniOS/net/unix_sock.h>
#include <miniOS/sched/sched.h>
#include <miniOS/io.h>
#include <string.h>

/* Static pool of AF_UNIX sockets */
static unix_sock_t unix_pool[UNIX_SOCK_POOL_SIZE];

static int ringbuf_avail(unix_sock_t *s) {
    return (int)(s->buf_tail - s->buf_head);
}

static int ringbuf_free(unix_sock_t *s) {
    return UNIX_SOCK_BUF_SIZE - ringbuf_avail(s);
}

unix_sock_t *unix_sock_alloc(void)
{
    for (int i = 0; i < UNIX_SOCK_POOL_SIZE; i++) {
        if (!unix_pool[i].in_use) {
            memset(&unix_pool[i], 0, sizeof(unix_pool[i]));
            unix_pool[i].in_use    = 1;
            unix_pool[i].ref_count = 1;  /* caller (fd) holds one ref */
            unix_pool[i].state     = UNIX_STATE_FREE;
            unix_pool[i].lock      = (spinlock_t)SPINLOCK_INIT;
            return &unix_pool[i];
        }
    }
    return NULL;
}

void unix_sock_free(unix_sock_t *s)
{
    if (!s) return;
    s->in_use = 0;
    s->state  = UNIX_STATE_FREE;
    s->peer   = NULL;
    s->ref_count = 0;
}

int unix_sock_bind(unix_sock_t *s, const char *path)
{
    if (!s || !path) return -22;
    strncpy(s->path, path, UNIX_SOCK_PATH_MAX - 1);
    s->path[UNIX_SOCK_PATH_MAX - 1] = '\0';
    s->state = UNIX_STATE_BOUND;
    return 0;
}

int unix_sock_listen(unix_sock_t *s, int backlog)
{
    (void)backlog;
    if (!s) return -22;
    s->state = UNIX_STATE_LISTENING;
    s->backlog_head = 0;
    s->backlog_tail = 0;
    return 0;
}

unix_sock_t *unix_sock_accept(unix_sock_t *server)
{
    if (!server || server->state != UNIX_STATE_LISTENING) return NULL;

    /* Wait until there is a client in the backlog */
    struct thread *t = sched_current();
    while (server->backlog_head == server->backlog_tail) {
        if (t->pending_signals)
            return NULL;
        sched_yield();
    }

    /* Take client from backlog; drop backlog's reference */
    unix_sock_t *client = server->backlog[server->backlog_head % UNIX_BACKLOG_MAX];
    server->backlog_head++;
    client->ref_count--;  /* backlog ref released */

    /* Allocate server-side connection socket */
    unix_sock_t *conn = unix_sock_alloc();
    if (!conn) {
        if (client->ref_count <= 0) unix_sock_free(client);
        return NULL;
    }

    if (client->ref_count > 0) {
        /* Client fd still open: wire up bidirectional peer connection */
        conn->state   = UNIX_STATE_CONNECTED;
        conn->peer    = client;
        client->peer  = conn;
        client->state = UNIX_STATE_CONNECTED;
    } else {
        /* Client fd already closed (e.g. fork+waitpid pattern): drain any
         * pre-sent data from the client's buffer into conn's buffer, then
         * release the client socket. */
        int avail = ringbuf_avail(client);
        for (int i = 0; i < avail; i++) {
            conn->buf[conn->buf_tail & (UNIX_SOCK_BUF_SIZE - 1)] =
                client->buf[client->buf_head & (UNIX_SOCK_BUF_SIZE - 1)];
            conn->buf_tail++;
            client->buf_head++;
        }
        conn->state = UNIX_STATE_CONNECTED;
        conn->peer  = NULL;
        unix_sock_free(client);
    }

    return conn;
}

int unix_sock_connect(unix_sock_t *s, const char *path)
{
    if (!s || !path) return -22;

    /* Find the listening socket with matching path */
    unix_sock_t *server = NULL;
    for (int i = 0; i < UNIX_SOCK_POOL_SIZE; i++) {
        if (unix_pool[i].in_use &&
            unix_pool[i].state == UNIX_STATE_LISTENING &&
            strncmp(unix_pool[i].path, path, UNIX_SOCK_PATH_MAX) == 0) {
            server = &unix_pool[i];
            break;
        }
    }
    if (!server) return -111; /* ECONNREFUSED */

    /* Check backlog space */
    int used = server->backlog_tail - server->backlog_head;
    if (used >= UNIX_BACKLOG_MAX) return -111;

    /* Enqueue self in server's backlog; backlog holds a reference */
    s->ref_count++;
    server->backlog[server->backlog_tail % UNIX_BACKLOG_MAX] = s;
    server->backlog_tail++;
    s->state = UNIX_STATE_CONNECTING;

    /* Non-blocking: return immediately.  Data written before accept() is
     * buffered in s->buf and copied to the conn socket by unix_sock_accept(). */
    return 0;
}

int unix_sock_write(unix_sock_t *s, const void *buf, size_t len)
{
    if (!s) return -9;

    /* CONNECTING: buffer data in own ring (will be drained by accept) */
    unix_sock_t *dst = (s->state == UNIX_STATE_CONNECTING) ? s : s->peer;
    if (!dst) return -32; /* EPIPE */

    size_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (written < len) {
        int free = ringbuf_free(dst);
        if (free <= 0) {
            sched_yield();
            if (sched_current()->pending_signals) return (written > 0) ? (int)written : -4;
            continue;
        }
        size_t to_write = (size_t)free < (len - written) ? (size_t)free : (len - written);
        for (size_t i = 0; i < to_write; i++) {
            dst->buf[dst->buf_tail & (UNIX_SOCK_BUF_SIZE - 1)] = src[written + i];
            dst->buf_tail++;
        }
        written += to_write;
    }
    return (int)written;
}

int unix_sock_read(unix_sock_t *s, void *buf, size_t len)
{
    if (!s) return -9;

    /* Block until data available */
    struct thread *t = sched_current();
    while (ringbuf_avail(s) == 0) {
        if (t->pending_signals)
            return -4; /* EINTR */
        /* If peer disconnected and no data, return EOF */
        if (!s->peer || s->peer->state != UNIX_STATE_CONNECTED)
            return 0; /* EOF */
        sched_yield();
    }

    int avail = ringbuf_avail(s);
    size_t to_read = (size_t)avail < len ? (size_t)avail : len;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = s->buf[s->buf_head & (UNIX_SOCK_BUF_SIZE - 1)];
        s->buf_head++;
    }
    return (int)to_read;
}

int unix_sock_socketpair(unix_sock_t **a, unix_sock_t **b)
{
    unix_sock_t *sa = unix_sock_alloc();
    if (!sa) return -24;
    unix_sock_t *sb = unix_sock_alloc();
    if (!sb) { unix_sock_free(sa); return -24; }

    sa->state = UNIX_STATE_CONNECTED;
    sb->state = UNIX_STATE_CONNECTED;
    sa->peer  = sb;
    sb->peer  = sa;

    *a = sa;
    *b = sb;
    return 0;
}

int unix_sock_close(unix_sock_t *s)
{
    if (!s) return 0;
    /* Notify peer that we're gone */
    if (s->peer) {
        s->peer->peer  = NULL;
        s->peer->state = UNIX_STATE_FREE; /* signal EOF to reader */
        s->peer = NULL;
    }
    /* Ref-counted free: only release pool slot when last reference drops */
    s->ref_count--;
    if (s->ref_count <= 0)
        unix_sock_free(s);
    return 0;
}

int unix_sock_has_data(unix_sock_t *s)
{
    if (!s) return 0;
    return ringbuf_avail(s) > 0;
}
