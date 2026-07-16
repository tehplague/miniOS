// MIT License
// Copyright (c) 2026 Christian Spoo

/*
 * testpthread — mlibc pthreads smoke tests
 *
 * Tests:
 *   pthread_create_join:     create a thread, join it, verify it ran
 *   pthread_mutex:           mutex lock/unlock guards a shared counter
 *   pthread_create_many:     create 4 threads concurrently; all must complete
 */

#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

static void write_str(const char *s) { write(1, s, strlen(s)); }

/* ── pthread_create / join ───────────────────────────────────────────────── */

static void *thread_set_flag(void *arg) {
    volatile int *flag = (volatile int *)arg;
    *flag = 1;
    return NULL;
}

static void test_pthread_create_join(void) {
    volatile int flag = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_set_flag, (void *)&flag) != 0) {
        write_str("FAIL: pthread_create_join: pthread_create failed\n");
        return;
    }
    pthread_join(t, NULL);
    if (flag == 1)
        write_str("PASS: pthread_create_join\n");
    else
        write_str("FAIL: pthread_create_join: flag not set by thread\n");
}

/* ── pthread_mutex ───────────────────────────────────────────────────────── */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_counter = 0;

static void *thread_increment(void *arg) {
    int n = *(int *)arg;
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&g_mutex);
        g_counter++;
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

static void test_pthread_mutex(void) {
    g_counter = 0;
    int n = 1000;
    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread_increment, &n);
    pthread_create(&t2, NULL, thread_increment, &n);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    if (g_counter == 2000)
        write_str("PASS: pthread_mutex\n");
    else
        write_str("FAIL: pthread_mutex: counter != 2000 (data race?)\n");
}

/* ── multiple threads ────────────────────────────────────────────────────── */

#define N_THREADS 4
static volatile int done_flags[N_THREADS];

static void *thread_mark_done(void *arg) {
    int idx = (int)(intptr_t)arg;
    done_flags[idx] = 1;
    return NULL;
}

static void test_pthread_create_many(void) {
    pthread_t threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        done_flags[i] = 0;
        pthread_create(&threads[i], NULL, thread_mark_done, (void *)(intptr_t)i);
    }
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(threads[i], NULL);

    int all_done = 1;
    for (int i = 0; i < N_THREADS; i++)
        if (!done_flags[i]) { all_done = 0; break; }

    if (all_done)
        write_str("PASS: pthread_create_many\n");
    else
        write_str("FAIL: pthread_create_many: not all threads completed\n");
}

int main(void) {
    write_str("=== testpthread ===\n");
    test_pthread_create_join();
    test_pthread_mutex();
    test_pthread_create_many();
    write_str("=== testpthread done ===\n");
    return 0;
}
