# Asynchronous requests

A `riak_async` handle runs any number of get, put, delete and ping requests
concurrently on one thread, using libcurl's multi interface. It takes the
same `riak_config` as a blocking client, including nodes, retries, security
and hooks.

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

- `res->errmsg` is valid only during the callback.
- `res->object` (get, and put with `return_body`) and `res->created_key`
  (put without a key) are freed when the callback returns, unless they are
  kept by copying them and clearing the originals:

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

## Scope

The asynchronous API covers object get, put, delete and ping. Queries, data
types and bucket operations are available through the blocking API, which
can run on separate threads with one client each.
