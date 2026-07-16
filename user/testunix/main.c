// MIT License
// Copyright (c) 2026 Christian Spoo

/*
 * testunix — AF_UNIX SOCK_STREAM integration tests
 *
 * Tests:
 *   unix_socketpair:       socketpair() creates connected pair; send/recv work
 *   unix_socketpair_echo:  bidirectional echo over socketpair
 *   unix_bind_listen:      fork: child connects, parent accepts; data flows both ways
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

static void write_str(const char *s) { write(1, s, strlen(s)); }

/* ── socketpair ──────────────────────────────────────────────────────────── */

static void test_unix_socketpair(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        write_str("FAIL: unix_socketpair: socketpair() failed\n");
        return;
    }

    const char *msg = "hello";
    send(sv[0], msg, 5, 0);

    char buf[16] = {0};
    int n = (int)recv(sv[1], buf, sizeof(buf), 0);
    if (n == 5 && memcmp(buf, "hello", 5) == 0)
        write_str("PASS: unix_socketpair\n");
    else
        write_str("FAIL: unix_socketpair: wrong data\n");

    close(sv[0]);
    close(sv[1]);
}

static void test_unix_socketpair_echo(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        write_str("FAIL: unix_socketpair_echo: socketpair() failed\n");
        return;
    }

    send(sv[0], "ping", 4, 0);
    char buf[8] = {0};
    recv(sv[1], buf, 4, 0);
    send(sv[1], buf, 4, 0);   /* echo back */
    char echo[8] = {0};
    int n = (int)recv(sv[0], echo, 8, 0);
    if (n == 4 && memcmp(echo, "ping", 4) == 0)
        write_str("PASS: unix_socketpair_echo\n");
    else
        write_str("FAIL: unix_socketpair_echo: wrong echo\n");

    close(sv[0]);
    close(sv[1]);
}

/* ── bind / listen / connect / accept ────────────────────────────────────── */

static void test_unix_bind_listen(void) {
    const char *path = "/tmp/test_unix.sock";

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { write_str("FAIL: unix_bind_listen: socket() failed\n"); return; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        write_str("FAIL: unix_bind_listen: bind() failed\n");
        close(server);
        return;
    }
    if (listen(server, 1) != 0) {
        write_str("FAIL: unix_bind_listen: listen() failed\n");
        close(server);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        /* child: connect (non-blocking) and send, then exit */
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un caddr = {0};
        caddr.sun_family = AF_UNIX;
        strncpy(caddr.sun_path, path, sizeof(caddr.sun_path) - 1);
        connect(c, (struct sockaddr *)&caddr, sizeof(caddr));
        send(c, "world", 5, 0);
        close(c);
        _exit(0);
    }

    /* Wait for child to connect+send+exit, THEN accept from the backlog.
     * miniOS fork is sequential (single PML4): child only runs after waitpid;
     * accept() after waitpid finds the already-queued connection. */
    int status = 0;
    waitpid(child, &status, 0);

    int peer = accept(server, NULL, NULL);
    if (peer < 0) {
        write_str("FAIL: unix_bind_listen: accept() failed\n");
        close(server);
        return;
    }
    char buf[16] = {0};
    int n = (int)recv(peer, buf, sizeof(buf), 0);
    if (n == 5 && memcmp(buf, "world", 5) == 0)
        write_str("PASS: unix_bind_listen\n");
    else
        write_str("FAIL: unix_bind_listen: wrong data\n");

    close(peer);
    close(server);
}

int main(void) {
    write_str("=== testunix ===\n");
    test_unix_socketpair();
    test_unix_socketpair_echo();
    test_unix_bind_listen();
    write_str("=== testunix done ===\n");
    return 0;
}
