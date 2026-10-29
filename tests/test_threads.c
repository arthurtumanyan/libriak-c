/*
 File:   test_threads.c
 SPDX-License-Identifier: GPL-3.0-or-later

 Clients used concurrently, one per thread, as the threading rules of
 riak.h allow: Protocol Buffers against the in-process stub of stub_pb.h
 and, when run through stub_riak.py (RIAK_TEST_PORT set), HTTP against the
 Python stub, whose store keeps what each thread writes. Every result is
 checked, so a response or error message crossing between clients fails the
 test; built with -DRIAK_SANITIZE=thread, ThreadSanitizer checks the
 library for data races.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "riak/riak.h"
#include "stub_pb.h"

#define THREADS 8
#define ROUNDS 25

static unsigned pb_port, http_port;
static atomic_int failures;

#define T_CHECK(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            atomic_fetch_add(&failures, 1);                                                        \
        }                                                                                          \
    } while (0)

typedef struct {
    riak_client *c;
    size_t n;
    riak_status nested;
} walk;

static bool on_key(const char *name, size_t len, void *ctx) {
    walk *w = ctx;
    T_CHECK(len > 0 && name[len] == '\0');
    if (w->n++ == 0) {
        w->nested = riak_ping(w->c); /* refused by the busy guard of this client only */
    }
    return true;
}

static riak_client *new_client(riak_transport t, unsigned port) {
    riak_config cfg;
    riak_config_init(&cfg);
    cfg.transport = t;
    cfg.port = port;
    cfg.max_retries = 0;
    riak_client *c = NULL;
    T_CHECK(riak_client_new(&cfg, &c) == RIAK_OK && c != NULL);
    return c;
}

static void *pb_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    riak_client *c = NULL;
    for (int r = 0; r < ROUNDS; r++) {
        /* odd threads create a client per round: OpenSSL and libcurl state
         * set up and torn down while other threads use theirs */
        if (c == NULL || id % 2 == 1) {
            riak_client_free(c);
            if ((c = new_client(RIAK_TRANSPORT_PB, pb_port)) == NULL) {
                return NULL;
            }
        }
        T_CHECK(riak_ping(c) == RIAK_OK);
        riak_object o;
        T_CHECK(riak_get(c, "b", "k", NULL, &o) == RIAK_OK);
        T_CHECK(o.data && o.len == 5 && memcmp(o.data, "hello", 5) == 0);
        riak_object_free(&o);
        char key[32];
        snprintf(key, sizeof key, "missing");
        T_CHECK(riak_get(c, "b", key, NULL, &o) == RIAK_E_NOT_FOUND);
        riak_strlist l;
        T_CHECK(riak_list_keys(c, "b", &l) == RIAK_OK && l.count == 3);
        if (l.count == 3) {
            T_CHECK(strcmp(l.items[2], "n3") == 0);
        }
        riak_strlist_free(&l);
        walk w = {.c = c, .nested = RIAK_OK};
        T_CHECK(riak_list_keys_stream(c, NULL, "b", on_key, &w) == RIAK_OK);
        T_CHECK(w.n == 3 && w.nested == RIAK_E_INVALID_ARG);
        /* an error message belongs to the client that failed */
        char bad[32];
        snprintf(bad, sizeof bad, "thread-%d", id);
        T_CHECK(riak_get(c, "", bad, NULL, &o) == RIAK_E_INVALID_ARG);
        T_CHECK(strstr(riak_client_errmsg(c), "bucket") != NULL);
        T_CHECK(strcmp(riak_strerror(RIAK_E_NOT_FOUND), "not found") == 0);
    }
    riak_client_free(c);
    return NULL;
}

static void *http_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    riak_client *c = new_client(RIAK_TRANSPORT_HTTP, http_port);
    if (c == NULL) {
        return NULL;
    }
    char bucket[32];
    snprintf(bucket, sizeof bucket, "threads-%d", id);
    for (int r = 0; r < ROUNDS; r++) {
        char key[32], value[64];
        snprintf(key, sizeof key, "k%d", r);
        snprintf(value, sizeof value, "thread %d round %d", id, r);
        T_CHECK(riak_put(c, bucket, key, value, strlen(value), NULL, NULL, NULL) == RIAK_OK);
        riak_object o;
        T_CHECK(riak_get(c, bucket, key, NULL, &o) == RIAK_OK);
        /* a response meant for another thread's client would differ */
        T_CHECK(o.data && o.len == strlen(value) && memcmp(o.data, value, o.len) == 0);
        riak_object_free(&o);
        riak_strlist l;
        T_CHECK(riak_list_keys(c, bucket, &l) == RIAK_OK && l.count == (size_t)r + 1);
        riak_strlist_free(&l);
        walk w = {.c = c, .nested = RIAK_OK};
        T_CHECK(riak_list_keys_stream(c, NULL, bucket, on_key, &w) == RIAK_OK);
        T_CHECK(w.n == (size_t)r + 1 && w.nested == RIAK_E_INVALID_ARG);
    }
    riak_client_free(c);
    return NULL;
}

static void run(void *(*fn)(void *), const char *what) {
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
        T_CHECK(pthread_create(&t[i], NULL, fn, (void *)(intptr_t)i) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
    }
    fprintf(stderr, "%s %s\n", atomic_load(&failures) == 0 ? "ok  " : "FAIL", what);
}

int main(void) {
    (void)stub_start_tls; /* TLS is covered by test_pb */
    pb_port = stub_start();
    if (riak_global_init() != RIAK_OK) {
        return 1;
    }
    run(pb_worker, "protocol buffers clients on 8 threads");
    const char *p = getenv("RIAK_TEST_PORT");
    if (p) {
        http_port = (unsigned)strtoul(p, NULL, 10);
        run(http_worker, "http clients on 8 threads");
    } else {
        fprintf(stderr, "skip http: RIAK_TEST_PORT not set\n");
    }
    riak_global_cleanup();
    return atomic_load(&failures) == 0 ? 0 : 1;
}
