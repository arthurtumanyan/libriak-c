# Logging and metrics

Both callbacks are set in `riak_config` and run on the thread that issued the
request (for asynchronous requests, the thread driving the handle).

## Logging

```c
static void my_log(riak_log_level level, const char *msg, void *ctx) {
    static const char *names[] = {"debug", "info", "warn", "error"};
    fprintf(stderr, "[riak %s] %s\n", names[level], msg);
}

cfg.log_fn = my_log;
cfg.log_level = RIAK_LOG_WARN;   /* default */
```

| Level | Messages |
|---|---|
| `RIAK_LOG_DEBUG` | Every HTTP attempt: method, URL, attempt number |
| `RIAK_LOG_WARN` | Transport failures and HTTP 503 responses that are retried; node ejections |
| `RIAK_LOG_ERROR` | Transport failures and HTTP 503 responses after which no retry follows |

Other outcomes (HTTP 404, TLS errors, invalid arguments) are reported only
through the return value and `riak_client_errmsg()`.

The message string is valid only during the call.

## Metrics

`metrics_fn` is called once per HTTP attempt, retries included, with a
`riak_request_metrics`:

| Field | Meaning |
|---|---|
| `method`, `path` | Request method, path and query |
| `host`, `port` | Node the attempt was sent to |
| `attempt` | 0 for the first try, then 1, 2, ... |
| `status` | `RIAK_OK` when a response arrived, otherwise the transport/TLS error |
| `http_status` | HTTP status, 0 without a response |
| `duration_ms` | Time from sending to completion |
| `bytes_received` | Size of the response body |
| `will_retry` | Whether another attempt follows |

```c
static void my_metrics(const riak_request_metrics *m, void *ctx) {
    histogram_observe(ctx, m->method, m->http_status, m->duration_ms);
}
cfg.metrics_fn = my_metrics;
cfg.metrics_ctx = registry;
```

Per-node counters are also available at any time through
`riak_client_node_info()`, see [Clusters and retries](clusters.md).
