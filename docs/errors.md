# Errors and ownership

## Status codes

Every call returns a `riak_status`. `riak_strerror()` gives a short
description, `riak_client_errmsg()` the detail of the last failure on a
client, and `riak_client_http_status()` the HTTP status of the last request
(always 0 over Protocol Buffers).
With more than one node, error details start with the `host:port` of the node
that answered.

| Status | Cause |
|---|---|
| `RIAK_OK` | Success |
| `RIAK_E_INVALID_ARG` | NULL or empty argument, invalid option or quorum, CR/LF in a header value |
| `RIAK_E_NOMEM` | Allocation failure |
| `RIAK_E_TRANSPORT` | Connection refused, DNS failure, timeout, connection reset |
| `RIAK_E_TLS` | Certificate or handshake failure, missing or rejected client certificate |
| `RIAK_E_NOT_FOUND` | HTTP 404, or a missing object, bucket type or search index |
| `RIAK_E_SIBLINGS` | HTTP 300 without a resolver, or a resolver that declined |
| `RIAK_E_PRECONDITION` | HTTP 412, a failed `if_none_match = "*"` or `if_not_modified`, or a data type removal of an absent element without context |
| `RIAK_E_HTTP` | Other HTTP status, or a response larger than `max_body_bytes` |
| `RIAK_E_PARSE` | Malformed response body |
| `RIAK_E_NOT_MODIFIED` | HTTP 304, or `if_modified` with an unchanged vector clock |
| `RIAK_E_UNAUTHORIZED` | HTTP 401, 403 or 426 (security enabled, plain HTTP), failed authentication, permission denied, a request without STARTTLS while security is enabled |
| `RIAK_E_CANCELLED` | Async request dropped by `riak_async_free()` |
| `RIAK_E_UNSUPPORTED` | Operation or option not available on the transport or the node, see [Transports](transports.md) |
| `RIAK_E_SERVER` | Protocol Buffers: any other error message from Riak |

Deleting a key that does not exist is reported as `RIAK_OK`.

## Ownership

- Results returned through out-parameters belong to the caller and are
  released with the matching function: `riak_object_free()`,
  `riak_object_set_free()`, `riak_strlist_free()`, `riak_index_result_free()`,
  `riak_search_result_free()`, `riak_datatype_free()`,
  `riak_server_info_free()`, `riak_preflist_free()`, `riak_coverage_free()`,
  and `riak_free()` for plain strings (JSON documents, generated keys).
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
- A callback of a streaming call (`riak_list_keys_stream()`,
  `riak_index_query_stream()`, `riak_mapred_stream()` and the like) does
  not issue requests on the client running it, because that client's
  connection is busy with the stream. Such a request returns
  `RIAK_E_INVALID_ARG` ("the client is busy") without touching the
  connection; a second client, or a `riak_async` handle, serves nested
  requests. `riak_client_free()` called from a callback is ignored and
  logged, since the client is still in use.

## Input validation

Bucket, key, bucket type and index names must not be empty; they are
percent-encoded in URLs, so any byte sequence is accepted. Header values
(content types, vclocks, metadata, conditional headers) are rejected with
`RIAK_E_INVALID_ARG` when they contain CR or LF, and metadata and index names
must be valid HTTP header tokens.
