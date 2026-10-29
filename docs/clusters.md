# Clusters and retries

## Multiple nodes

```c
riak_node nodes[] = {{"10.0.0.1", 8098}, {"10.0.0.2", 8098}, {"10.0.0.3", 8098}};
riak_config cfg;
riak_config_init(&cfg);
cfg.nodes = nodes;
cfg.n_nodes = 3;
cfg.balance = RIAK_BALANCE_ROUND_ROBIN;
```

- `RIAK_BALANCE_ROUND_ROBIN` spreads requests over the healthy nodes.
- `RIAK_BALANCE_FAILOVER` always sends to the first healthy node, in the
  order given.

Node selection, retries and health tracking apply to every call, blocking
and asynchronous.

## Retries

A failed attempt is retried on the next node after a backoff, up to
`max_retries` additional attempts. The delay starts at `retry_base_ms`,
doubles with each retry up to `retry_max_ms`, and is spread with "equal
jitter" (half fixed, half random).

| Failure | Repeatable requests | Other requests |
|---|---|---|
| Connection refused, DNS failure (request never sent) | retried | retried |
| Transport error after sending (reset, timeout) | retried | not retried |
| HTTP 503, or an overload or timeout error over Protocol Buffers | retried | not retried |
| Any other HTTP status or server error | not retried | not retried |
| TLS and authentication errors | not retried | not retried |

Repeatable requests are reads, puts and deletes of a named key, listings and
queries (HTTP GET, PUT and DELETE). Puts with a generated key, data type
updates such as counter increments and MapReduce (HTTP POST) are not
repeated once they may have reached a node, so nothing is applied twice.
The rules are the same on both transports. Streamed responses (key listing, streamed index
queries, streamed MapReduce) are not restarted once data was delivered.

Setting `max_retries = 0` disables retries.

## Node health

A node is ejected after `fail_threshold` consecutive failed attempts and
skipped for `eject_ms`; it is tried again afterwards, and a success restores
it. When every node is ejected, the node that comes back first is used.

```c
for (size_t i = 0; i < riak_client_node_count(c); i++) {
    riak_node_info n;
    riak_client_node_info(c, i, &n);
    printf("%s:%u %s, %llu requests, %llu failures\n", n.host, n.port,
           n.healthy ? "healthy" : "ejected", n.requests, n.failures);
}
size_t up = riak_client_check_nodes(c);   /* pings every node now */
```
