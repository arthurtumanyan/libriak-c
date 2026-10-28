# Configuration

A `riak_config` is filled with defaults by `riak_config_init()` and then
adjusted. `riak_client_new()` and `riak_async_new()` copy every string, so
the structure may be discarded after the call. Passing `NULL` selects the
defaults.

## Connection

| Field | Default | Meaning |
|---|---|---|
| `transport` | `RIAK_TRANSPORT_PB` | Wire protocol, see [Transports](transports.md) |
| `host` | `"127.0.0.1"` | Node address; ignored when `nodes` is set |
| `port` | `0` | Node port; 0 selects 8087 (Protocol Buffers) or 8098 (HTTP); ignored when `nodes` is set |
| `http_port` | `0` | Protocol Buffers: HTTP port used by `riak_stats()`; 0 means none |
| `tls` | `false` | HTTPS, or STARTTLS on a Protocol Buffers connection |
| `tls_verify` | `true` | The server certificate and host name are verified |
| `ca_file` | `NULL` | CA bundle (PEM); `NULL` uses the system store |
| `connect_timeout_ms` | `0` | Connection timeout; 0 keeps the default of 300 s |
| `timeout_ms` | `0` | Whole-request timeout; 0 means no limit |
| `client_id` | `NULL` | Sent as `X-Riak-ClientId`, or with `RpbSetClientIdReq` on each new connection |
| `max_body_bytes` | 64 MiB | Larger responses (per message on streams) fail with `RIAK_E_HTTP` |
| `pb_max_connections_per_node` | `0` | Async Protocol Buffers requests: connections per node; 0 selects 8 |

## Cluster and retries

| Field | Default | Meaning |
|---|---|---|
| `nodes`, `n_nodes` | none | Cluster nodes (`host`, `port`, `http_port`); replace `host`/`port` when set |
| `balance` | `RIAK_BALANCE_ROUND_ROBIN` | Or `RIAK_BALANCE_FAILOVER` |
| `max_retries` | `2` | Additional attempts after a retryable failure |
| `retry_base_ms` | `50` | First backoff delay |
| `retry_max_ms` | `2000` | Backoff cap |
| `fail_threshold` | `3` | Consecutive failures that eject a node |
| `eject_ms` | `5000` | Time an ejected node is skipped |

The retry rules are described in [Clusters and retries](clusters.md).

## Security

| Field | Meaning |
|---|---|
| `username`, `password` | Riak security user (HTTP Basic, or `RpbAuthReq` after STARTTLS); refused unless `tls` is set |
| `tls_cert_file`, `tls_key_file` | Client certificate and key (PEM) |
| `tls_key_password` | Passphrase of an encrypted key |
| `tls_legacy` | Interoperability with nodes on an old Erlang/OTP TLS stack (Riak KV 2.2 and older); weakens TLS, see [Security](security.md) |

See [Security](security.md).

## Logging and metrics

| Field | Default | Meaning |
|---|---|---|
| `log_fn`, `log_ctx` | none | Log callback |
| `log_level` | `RIAK_LOG_WARN` | Minimum level passed to `log_fn` |
| `metrics_fn`, `metrics_ctx` | none | Called once per attempt |

See [Logging and metrics](observability.md).

## Validation

`riak_client_new()` returns `RIAK_E_INVALID_ARG` when a host contains
characters that do not belong in a host name, a port exceeds 65535,
`client_id` contains CR or LF, `n_nodes` is set without `nodes`, a negative
delay is given, `balance`, `transport` or `log_level` is out of range, or
credentials are given without `tls`.

## Per-request options

Object operations take a `riak_opts` and data type operations a
`riak_dt_opts`. A zero-initialized structure, or `NULL`, selects the server
defaults. Quorum fields accept `RIAK_Q_DEFAULT`, `RIAK_Q_ONE`,
`RIAK_Q_QUORUM`, `RIAK_Q_ALL` or a positive replica count. The remaining
fields are described in [Objects and buckets](objects.md).
