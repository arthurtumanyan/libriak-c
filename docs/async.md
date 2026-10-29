# Asynchronous requests

A `riak_async` handle runs any number of requests concurrently on one
thread: over Protocol Buffers with a pool of
non-blocking connections per node (at most
`riak_config::pb_max_connections_per_node`, 8 by default; further requests
wait for a free connection), over HTTP with libcurl's multi interface. It
takes the same `riak_config` as a blocking client, including the transport,
nodes, retries, security and hooks.

## Built-in loop

```c
static void done(riak_async_result *res, void *ctx) {
    if (res->status == RIAK_OK && res->object.data)
        printf("%.*s\n", (int)res->object.len, res->object.data);
    else if (res->status != RIAK_OK)
        fprintf(stderr, "%s: %s\n", riak_strerror(res->status), res->errmsg);
}

riak_async *a;
riak_async_new(&cfg, &a);
riak_async_get(a, "users", "u1", NULL, done, NULL);
riak_async_put(a, "users", "u2", "{}", 2, &opts, done, NULL);
size_t left = riak_async_run(a, 5000);    /* ms; -1 for no limit */
riak_async_free(a);
```

- Submission validates the arguments and copies what is needed later,
  including the value of a put. Argument errors are returned directly and
  the callback is not called.
- `riak_async_run()` returns when nothing is pending or the time limit has
  passed, with the number of requests still pending.
- Retries are scheduled as timers; the loop never sleeps.
- `riak_async_free()` completes pending requests with `RIAK_E_CANCELLED`.

## Callbacks

- `res->errmsg` is valid only during the callback; `res->kind` names the
  request.
- The result members (`object`, `siblings`, `datatype`, `index`, `search`,
  `json`, `counter`, `created_key`, `continuation`, see the table below)
  are freed when the callback returns, unless they are kept by copying them
  and clearing the originals:

```c
static void keep(riak_async_result *res, void *ctx) {
    riak_object *dst = ctx;
    *dst = res->object;
    memset(&res->object, 0, sizeof res->object);
}
```

- New requests may be submitted from a callback; `riak_async_free()` is not
  called from one.
- `riak_async_client()` gives access to the underlying client, for example
  for `riak_client_node_info()`.

## External event loops

For libuv, libevent, epoll or similar, the handle reports which descriptors
to watch and when a timer is due; the loop reports readiness back:

```c
static void on_socket(int fd, int events, void *ctx) {
    /* RIAK_POLL_IN / RIAK_POLL_OUT: watch fd; RIAK_POLL_REMOVE: stop watching */
}
static void on_timer(long timeout_ms, void *ctx) {
    /* call riak_async_on_timeout() after timeout_ms; -1 cancels the timer */
}

riak_async_set_loop(a, on_socket, on_timer, loop_ctx);   /* before the first request */
/* when fd becomes ready:  riak_async_on_socket(a, fd, RIAK_POLL_IN | RIAK_POLL_OUT); */
/* when the timer fires:   riak_async_on_timeout(a); */
```

`riak_async_run()` is not used once a loop is set. A complete `poll()`-based
loop is found in `tests/test_async.c`, and `examples/async.c` writes many keys
concurrently with the built-in loop.

## Calls

| Call | Result in `riak_async_result` |
|---|---|
| `riak_async_ping()` | — |
| `riak_async_get()` | `object` |
| `riak_async_put()` | `object` with `return_body`; `created_key` without a key |
| `riak_async_delete()` | — |
| `riak_async_get_siblings()` | `siblings` |
| `riak_async_dt_fetch()` | `datatype` |
| `riak_async_dt_update()` | `datatype` with `return_body`; `created_key` without a key |
| `riak_async_counter_get()`, `riak_async_counter_update()` | `counter` |
| `riak_async_index_query()` | `index` (one page, with its continuation) |
| `riak_async_index_query_stream()` | each match goes to the item callback; `continuation` |
| `riak_async_search()` | `search` |
| `riak_async_mapred()` | `json` (the result array) |
| `riak_async_mapred_stream()` | each phase result goes to the item callback |
| `riak_async_get_bucket_props()` | `json` |
| `riak_async_set_bucket_props()` | — |
| `riak_async_list_keys_stream()`, `riak_async_list_buckets_stream()` | each name goes to the item callback |

Each call takes the arguments of its blocking counterpart, builds the same
request and interprets the answer the same way, on both transports; the
retry rules are the same too (reads are retried, counter and data type
updates and delivering streams are not). A data type operation is encoded
on submission and may be freed right after the call.

Streams have two callbacks: the item callback runs for every key, match or
phase result as it arrives, inside `riak_async_run()` or the loop hooks, and
returning `false` from it stops the stream (its connection is then closed,
and the request completes with `RIAK_OK`); the completion callback runs
once at the end.

```c
static bool on_key(const char *key, size_t len, void *ctx) {
    printf("%.*s\n", (int)len, key);
    return true;
}
riak_async_list_keys_stream(a, NULL, "users", on_key, NULL, done, NULL);
```

## Scope

Administration and operations calls stay blocking: Riak Search index and
schema administration, statistics, the Query API, AAE folds, replication
queues, link walking, object folds, preference lists and coverage plans.
They can run on separate threads with one client each.
