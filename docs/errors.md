# Errors and ownership

## Status codes

Every call returns a `riak_status`. `riak_strerror()` gives a short
description, `riak_client_errmsg()` the detail of the last failure on a
client, and `riak_client_http_status()` the HTTP status of the last request.
With more than one node, error details start with the `host:port` of the node
that answered.

| Status | Cause |
|---|---|
| `RIAK_OK` | Success |
| `RIAK_E_INVALID_ARG` | NULL or empty argument, invalid option or quorum, CR/LF in a header value |
| `RIAK_E_NOMEM` | Allocation failure |
| `RIAK_E_TRANSPORT` | Connection refused, DNS failure, timeout, connection reset |
| `RIAK_E_TLS` | Certificate or handshake failure, missing or rejected client certificate |
| `RIAK_E_NOT_FOUND` | HTTP 404 |
| `RIAK_E_SIBLINGS` | HTTP 300 without a resolver, or a resolver that declined |
| `RIAK_E_PRECONDITION` | HTTP 412, or a data type removal of an absent element without context |
| `RIAK_E_HTTP` | Other HTTP status, or a response larger than `max_body_bytes` |
| `RIAK_E_PARSE` | Malformed response body |
| `RIAK_E_NOT_MODIFIED` | HTTP 304 after a conditional get |
| `RIAK_E_UNAUTHORIZED` | HTTP 401 or 403 |
| `RIAK_E_CANCELLED` | Async request dropped by `riak_async_free()` |

Deleting a key that does not exist is reported as `RIAK_OK`.

## Ownership

- Results returned through out-parameters belong to the caller and are
  released with the matching function: `riak_object_free()`,
  `riak_object_set_free()`, `riak_strlist_free()`, `riak_index_result_free()`,
  `riak_search_result_free()`, `riak_datatype_free()`, and `riak_free()` for
  plain strings (JSON documents, generated keys).
- Out-parameters are cleared at the start of each call, so they are safe to
  free after a failure.
- Every free function accepts `NULL`.
- Input strings are copied when they are needed beyond the call (async
  submissions copy the request, including the value).
- String lists (`riak_strlist::items`, `riak_index_result::keys`,
  `riak_dt_value::elements`) are NULL-terminated and also carry a count.
- Object values (`riak_object::data`) are NUL-terminated for convenience;
  `len` gives the exact length, which matters for binary data.

## Threading

- `riak_global_init()` is called once before other threads are started, and
  `riak_global_cleanup()` once at the end.
- A `riak_client` is not used by two threads at the same time; one client
  per thread is created instead. Clients share nothing.
- A `riak_async` handle runs many concurrent requests on the thread that
  drives it.
- Log and metrics callbacks run on the thread that issued the request.

## Input validation

Bucket, key, bucket type and index names must not be empty; they are
percent-encoded in URLs, so any byte sequence is accepted. Header values
(content types, vclocks, metadata, conditional headers) are rejected with
`RIAK_E_INVALID_ARG` when they contain CR or LF, and metadata and index names
must be valid HTTP header tokens.
