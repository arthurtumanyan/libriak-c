# Changelog

## Unreleased

### Fixed
- A streaming callback (key or bucket listing, index query, MapReduce,
  object fold) that issued a request on the client running it reused the
  connection and buffers of the stream in progress: a use-after-free over
  Protocol Buffers and a stack use-after-return over HTTP. Such requests
  now return `RIAK_E_INVALID_ARG` without touching the connection, and
  `riak_client_free()` called from such a callback is ignored and logged
  instead of freeing the client in use.
- Connection errors are formatted with `strerror_r()` instead of the
  non-reentrant `strerror()`.

### Changed
- Riak Search calls return `RIAK_E_UNSUPPORTED` on a node without Search
  (Riak 3.x, `search = off`) on both transports; over HTTP they returned
  `RIAK_E_NOT_FOUND`. A query on a missing index is `RIAK_E_NOT_FOUND` on
  both transports (it was `RIAK_E_SERVER` over Protocol Buffers).
- `riak_get_preflist()` over HTTP returns `RIAK_E_UNSUPPORTED`: Riak 2.x's
  HTTP resource ignores the key and answers the same list for every key,
  and Riak 3.0 removed it. Protocol Buffers serves preference lists.
- A grow-only set update with `return_body` is sent as one request, the set
  coming in the update response. Riak 3.2 and 3.4 as released fail such
  requests over Protocol Buffers (a server bug, fixed and offered upstream;
  see `docs/operations.md`).
- zlib is required (AAE tree branches).
- `RIAK_E_UNSUPPORTED` also covers calls the node does not implement:
  Riak 2.2 "Unknown message code" answers and its HTML 404 for newer HTTP
  resources, Riak 3.x preference lists over HTTP, and link walking under
  Riak security.
- A Protocol Buffers connection on which Riak reported a crashed request
  handler is closed instead of reused.
- libcurl 7.66 or newer is required (`curl_multi_poll`).
- HTTP 426 (security enabled, request over plain HTTP) is reported as
  `RIAK_E_UNAUTHORIZED`, as the equivalent Protocol Buffers error is.
- Protocol Buffers is the default transport: `riak_config_init()` selects
  `RIAK_TRANSPORT_PB` and port 0, which means 8087 for Protocol Buffers and
  8098 for HTTP. HTTP is selected with `riak_config.transport`.
- protobuf-c 1.3 (with `protoc-c`) and OpenSSL 1.1.1 or newer are required.
- Retry decisions are taken by the operation rather than the HTTP method;
  the rules are unchanged and shared by both transports.

### Added
- Riak 2.9 to 3.4 features, on both transports where Riak serves them:
  - request options `node_confirms`, `sync_on_write`, `asis`,
    `return_head` and `deletedvclock`, `riak_index_query.pagination_sort`
    and `riak_dt_opts.node_confirms`;
  - bucket properties `node_confirms`, `sync_on_write`, `aae_tree_exclude`;
  - `riak_set_type_props()`, `riak_get_client_id()`, legacy counters
    (`riak_counter_update()`, `riak_counter_get()`);
  - object folds (`riak_fold_objects()`), link walking
    (`riak_link_walk()`) and the Riak 3.4 Query API (`riak_query_run()`,
    `riak_query_fetch()`);
  - AAE folds (`riak_aae_*`), nextgenrepl queues (`riak_repl_fetch()`,
    `riak_repl_push()`, `riak_cluster_membership()`) and riak_repl
    (`riak_get_cluster_id()`, `riak_repl_get()`).
  The `.proto` files are refreshed from OpenRiak riak_pb (`openriak-3.4`),
  and `riak_repl.proto` is added.
- Live tests against Riak KV 2.2.3, 3.2.6 and 3.4.1 (OpenRiak images built
  from source in `tests/docker/openriak`, node setup in
  `tests/docker/setup-node.sh`), plain and with TLS and authentication; a
  feature matrix in `docs/features.md`.
- `tests/test_threads.c` and `-DRIAK_SANITIZE=thread`.
- Protocol Buffers transport covering every public call: objects, siblings
  and resolvers, listings, bucket and bucket-type properties (as the same
  JSON documents as HTTP), secondary indexes, Riak Search and its
  administration, MapReduce, data types, the async API (connection pool per
  node, `riak_config.pb_max_connections_per_node`) and the external event
  loop hooks. STARTTLS with certificate and host name verification, client
  certificates and `RpbAuthReq` authentication; cluster balancing, retries
  and node ejection as over HTTP. The `.proto` files of basho/riak_pb are
  vendored in `proto/`.
- `riak_get_server_info()`, `riak_get_preflist()`,
  `riak_list_keys_stream()`, `riak_list_buckets_stream()` on both
  transports; `riak_get_coverage()` and `riak_index_query.cover_context` over
  Protocol Buffers.
- `riak_opts` fields `head`, `basic_quorum`, `notfound_ok` (both transports)
  and `sloppy_quorum`, `n_val`, `if_modified`, `if_not_modified` (Protocol
  Buffers).
- `riak_config.http_port` and `riak_node.http_port`, used by `riak_stats()`
  over Protocol Buffers.
- `riak_config.tls_legacy` for nodes on an old Erlang/OTP TLS stack
  (Riak KV 2.2 and older), which cannot complete a handshake with OpenSSL 3
  clients on either transport. Such handshake failures are reported as
  `RIAK_E_TLS` with a hint instead of a retryable transport error.
- Status codes `RIAK_E_UNSUPPORTED` and `RIAK_E_SERVER`;
  `riak_request_metrics.transport`.
- Tests: an in-process Protocol Buffers stub (`tests/test_pb.c`), live tests
  on both transports, and the `fuzz_pb` libFuzzer target.
- Bucket types: `riak_opts.bucket_type` for object calls and `riak_type_*`
  variants of the bucket calls; `riak_get_type_props()`.
- Siblings: `riak_get_siblings()` parses `multipart/mixed` responses into a
  `riak_object_set`; `riak_opts.resolver` lets `riak_get()` pick one sibling,
  returned with the vclock that resolves the conflict on write-back.
- Conditional requests: `if_none_match`, `if_match`, `if_modified_since`,
  `if_unmodified_since`; new status `RIAK_E_NOT_MODIFIED` for 304.
- `riak_object` now carries user metadata, secondary index entries, links and
  a tombstone flag.
- `riak_opts.server_timeout_ms` (Riak `timeout` parameter) and `riak_stats()`.
- Secondary index queries: exact and range matches, `$key`/`$bucket`,
  `return_terms`, `term_regex`, pagination with continuations
  (`riak_index_query_run()`) and streamed results through a callback
  (`riak_index_query_stream()`).
- Riak Search: `riak_search()` with filter, sort, field list, paging and
  parsed documents; index and schema administration.
- MapReduce: `riak_mapred()` and streamed phase results via
  `riak_mapred_stream()`.
- Streamed responses are parsed incrementally; a callback can stop a stream
  early, which aborts the transfer.
- Data types (CRDTs): `riak_dt_fetch()` and `riak_dt_update()` for counters,
  sets, maps (counter, set, map, register and flag fields, nested), HyperLogLogs
  and grow-only sets. Updates are built with `riak_dt_op_*`; misuse and
  allocation failures are reported by `riak_dt_update()`. Removing an absent
  element without a context maps to `RIAK_E_PRECONDITION`.
- Clusters: `riak_config.nodes` with round-robin or failover balancing,
  retries with exponential backoff and jitter, and per-node ejection after
  consecutive failures (circuit breaker). Only requests that are safe to repeat
  are retried after reaching a node. `riak_client_check_nodes()`,
  `riak_client_node_info()` and `riak_client_node_count()`; error messages name
  the node.
- Asynchronous API on curl's multi interface: `riak_async_get/put/delete/ping`
  with callbacks, a built-in loop (`riak_async_run()`) and hooks for external
  event loops (`riak_async_set_loop()`, `riak_async_on_socket()`,
  `riak_async_on_timeout()`). Failover and retries work as in the blocking API,
  with backoff as timers. Pending requests are cancelled on free.
- Riak security: HTTP Basic authentication (refused without TLS), client
  certificates with optional key passphrase; `RIAK_E_UNAUTHORIZED` for 401/403;
  a server that requires a client certificate is reported as `RIAK_E_TLS`.
- Observability: `log_fn` with levels and `metrics_fn` called once per HTTP
  attempt (node, attempt, outcome, status, duration, bytes, retry).
- Secrets are copied by the client and wiped when it is freed.
- `examples/async.c`.
- Documentation: guides in `docs/` (getting started, configuration, errors and
  ownership, objects, queries, data types, clusters, async, security,
  observability, testing, packaging, migration) and a Doxygen API reference
  with every public symbol documented (`cmake --build build --target docs`).
  CI fails on undocumented symbols, and a `docs` test checks that the API
  names used in the documentation exist and that example programs build.
- `tests/docker`: Riak image with the leveldb backend and Search enabled for
  the live tests.
- Debian and RPM packages via CPack (runtime and development components),
  `packaging/build-packages.sh` and a package smoke test.
- Release workflow: builds and install-tests packages on Ubuntu 22.04/24.04,
  Fedora and AlmaLinux 10, publishes them with a source tarball, checksums and
  build provenance attestations.
- CI: least-privilege permissions, actions pinned to commit SHAs, jobs for
  the oldest supported distribution, a live Riak node, coverage (fails under
  85 % lines), a fuzzing smoke run, a package dry run and actionlint; CodeQL
  analysis and Dependabot updates for actions.
- `SECURITY.md`, `CONTRIBUTING.md`, issue and pull request templates,
  `CODEOWNERS`.

## 2.0.0 — 2026-10-26

Complete rewrite. The public API is new and not source-compatible with 1.x;
see the migration table in the README.

### Fixed
- Library failed to link with GCC ≥ 10 (`-fno-common`): global variables were
  defined in the public header.
- A request method, body or header list set by one call stayed on the curl
  handle: after `RiakDeleteKey()` the next `RiakGetKey()` sent `DELETE`, and
  the header list pointed at freed memory.
- Heap overflow building the server URL (fixed 30-byte buffer).
- Heap overflow in bucket/key list parsing (`malloc(strlen(s))`).
- Off-by-one writes past every header buffer; `X-Riak-ClientId` header
  overflowed its buffer.
- Key/bucket lists were not NULL-terminated although the documented usage
  iterated until NULL.
- JSON was parsed on every partial network chunk (crash on split responses,
  leaks); streamed key listing (`keys=stream`) now works.
- NULL dereference when `content_type`, `metadata` or `index_entry` was unset.
- Ping read a non-terminated buffer and an uninitialised status.
- PUT without `returnbody` (HTTP 204) was reported as a failure.
- Auto-generated keys were truncated in the URL and never returned.
- Bucket and key names were not URL-encoded; header values were not checked
  for CR/LF injection.
- Quorum options (`r`, `w`, `dw`, `pr`, `pw`, `rw`) were accepted but never
  sent.
- Leaks: per-call response buffers, `/dev/null` file handle, server URL,
  header lists on early returns.

### Security
- TLS peer and host verification is now enabled by default
  (it was unconditionally disabled).

### Added
- `riak_get_bucket_props()` / `riak_set_bucket_props()` (were unimplemented).
- Status codes (`riak_status`) with per-client error messages.
- Response metadata: content type, vclock, ETag, Last-Modified.
- Timeouts, CA bundle, client-id and response size limit (`max_body_bytes`)
  configuration.
- CMake build with install/export, pkg-config file, unit and integration
  tests (stub Riak server and optional live Riak node), libFuzzer target,
  coverage and sanitizer builds, GitHub Actions CI.

### Removed
- NetBeans project files and committed prebuilt `.so` binaries.
