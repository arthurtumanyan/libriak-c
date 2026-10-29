# Transports

Riak is reached over one of two wire protocols, selected with
`riak_config::transport`:

| Transport | Default port | Notes |
|---|---|---|
| `RIAK_TRANSPORT_PB` (default) | 8087 | Riak's Protocol Buffers interface: binary messages on persistent TCP connections |
| `RIAK_TRANSPORT_HTTP` | 8098 | Riak's HTTP interface through libcurl |

```c
riak_config cfg;
riak_config_init(&cfg);                  /* Protocol Buffers, 127.0.0.1:8087 */
cfg.transport = RIAK_TRANSPORT_HTTP;     /* HTTP, 127.0.0.1:8098 */
```

A `port` of 0 (the default, also in `riak_node::port`) selects the default
port of the chosen transport. The public API is the same for both: every call
is available on both transports, and results have the same shape. Vector
clocks and data type contexts are base64 strings, ETags are quoted, dates use
the HTTP format, links use the `Link` header syntax, and bucket properties are
the `{"props":{...}}` JSON document. Vector clocks and contexts obtained over
one transport are therefore accepted by the other.

## Performance

Protocol Buffers avoids HTTP headers, URL encoding and JSON parsing, and
values travel as raw bytes. Against a local Riak KV 2.2 node, sequential gets
of 1 KiB objects run roughly 2.5 times faster than over HTTP, and puts
roughly 2 times faster.

## Connections

- Blocking calls keep one connection per node and reuse it across calls. A
  connection that the server closed while idle is detected before it is used
  and replaced, so the request is not lost.
- An async handle keeps a pool per node of at most
  `riak_config::pb_max_connections_per_node` connections (8 by default).
  Requests beyond that wait in a per-node queue. Riak answers one request at
  a time per connection, so requests are never pipelined.
- Node addresses are resolved when a connection is opened and cached for
  60 seconds. IPv4 and IPv6 addresses are tried in turn.
- `connect_timeout_ms` covers connecting, STARTTLS and authentication;
  `timeout_ms` covers each request from sending to the last response
  message.
- A stream that a callback stops early (key listing, index query, MapReduce)
  closes its connection, since the remaining messages are still on their
  way.

## Differences between the transports

| Feature | Protocol Buffers | HTTP |
|---|---|---|
| `riak_opts::if_none_match` / `if_match` with an ETag | `RIAK_E_UNSUPPORTED` | Supported |
| `riak_opts::if_modified_since`, `if_unmodified_since` | `RIAK_E_UNSUPPORTED` | Supported |
| `riak_opts::if_modified`, `if_not_modified` (vector clock) | Supported | `RIAK_E_UNSUPPORTED` |
| `riak_opts::sloppy_quorum`, `n_val` | Supported | `RIAK_E_UNSUPPORTED` |
| `riak_opts::if_none_match = "*"` | Supported | Supported |
| `riak_get_coverage()`, `riak_index_query::cover_context` | Supported | `RIAK_E_UNSUPPORTED` |
| `riak_opts::return_head`, `deletedvclock` | Supported | `RIAK_E_UNSUPPORTED` |
| `riak_opts::node_confirms` on a delete | `RIAK_E_UNSUPPORTED` | Supported |
| `riak_fold_objects()`, `riak_get_client_id()`, `riak_repl_push()` | Supported | `RIAK_E_UNSUPPORTED` |
| `riak_get_cluster_id()`, `riak_repl_get()` | Supported | `RIAK_E_UNSUPPORTED` |
| `riak_query_run()`, `riak_query_fetch()` | Through `http_port`, see below | Supported |
| `riak_get_preflist()` | Supported | `RIAK_E_UNSUPPORTED` (Riak's HTTP resource ignores the key) |
| `riak_stats()` | Through `http_port`, see below | Supported |
| `riak_link_walk()` | MapReduce link phases, then gets | Riak's link walker |
| `riak_set_bucket_props()` | Properties known to `RpbBucketProps`; others give `RIAK_E_INVALID_ARG` | Any property |
| `riak_client_http_status()` | Always 0 | HTTP status |
| Server errors | `RIAK_E_SERVER` with Riak's message | `RIAK_E_HTTP` with the status |

Conditional requests use the vector clock over Protocol Buffers:

```c
riak_opts cond = {.if_modified = true, .vclock = obj.vclock};
riak_get(c, "users", "u1", &cond, &again);   /* RIAK_E_NOT_MODIFIED if unchanged */

riak_opts cas = {.if_not_modified = true, .vclock = obj.vclock};
riak_put(c, "users", "u1", v, len, &cas, NULL, NULL);   /* RIAK_E_PRECONDITION if changed */
```

Which calls a given Riak version serves is listed in
[Features](features.md).

## Statistics and the Query API

Riak serves node statistics and the Query API only over HTTP. With the
Protocol Buffers transport, `riak_stats()`, `riak_query_run()` and
`riak_query_fetch()` are sent to the HTTP port given in
`riak_config::http_port` (or `riak_node::http_port` for clusters), on the
first node that has one; without one it returns `RIAK_E_UNSUPPORTED`.
`riak_get_server_info()` (node name and version) works on both transports.

## Coverage plans

`riak_get_coverage()` splits a bucket's key space into parts that can be
queried in parallel, for example on several threads or nodes:

```c
riak_coverage_plan plan;
if (riak_get_coverage(c, NULL, "users", 0, &plan) == RIAK_OK) {
    for (size_t i = 0; i < plan.count; i++) {
        riak_index_query q = {.index = "$bucket", .value = "users",
                              .cover_context = plan.entries[i].cover_context};
        /* riak_index_query_stream(c, "users", &q, cb, ctx, NULL) on each part */
    }
    riak_coverage_free(&plan);
}
```

Coverage plans are served only by Riak TS builds of riak_kv; every Riak KV
release answers "Unknown message code: 70", reported as
`RIAK_E_UNSUPPORTED`.

## Security

Over Protocol Buffers, TLS is started with STARTTLS on the same port and
authentication follows the handshake. Riak accepts STARTTLS only when Riak
security is enabled. Nodes on an old Erlang/OTP (Riak KV 2.2) need
`riak_config::tls_legacy` on both transports; see [Security](security.md).
