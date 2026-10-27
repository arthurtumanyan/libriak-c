# Migrating from 1.x

Version 2.0 is a complete rewrite and not source-compatible with 1.x. The
global variables of 1.x (`get_key_op_result`, `list_bucket_op_result`,
`rserver_url`) no longer exist; every result is returned through an
out-parameter and released by the caller.

| 1.x | 2.0 |
|---|---|
| `RiakInit(host, port, ssl)` | `riak_config_init()` + `riak_client_new()` |
| `RiakClose(h)` | `riak_client_free(c)` |
| `RiakPing(h)` → `bool` | `riak_ping(c)` → `riak_status` |
| `RiakGetKey(h, b, k)` + `RiakFreeMem(get_key_op_result.body)` | `riak_get(c, b, k, opts, &obj)` + `riak_object_free(&obj)` |
| `RiakSetKey(h, b, k, v, &op)` | `riak_put(c, b, k, v, len, &opts, out, created_key)` |
| `RiakDeleteKey(h, b, k, &op)` | `riak_delete(c, b, k, &opts)` |
| `RiakListBuckets(h)` | `riak_list_buckets(c, &list)` |
| `RiakListKeys(h, b, stream)` | `riak_list_keys(c, b, &list)` (always streamed) |
| `RiakGetBucketProps(b)` (stub) | `riak_get_bucket_props(c, b, &json)` |
| `RiakResetBucketProps(h, b)` | `riak_reset_bucket_props(c, b)` |
| `_riak_optional_p` | `riak_opts` |
| `bool` / `int` results | `riak_status` with `riak_client_errmsg()` |

Behaviour changes:

- TLS certificates are verified by default (1.x disabled verification).
- A put without `return_body` that receives HTTP 204 is a success (1.x
  reported a failure).
- Lists are NULL-terminated and also carry a count.
- Bucket and key names are URL-encoded.

The defects fixed by the rewrite are listed in `CHANGELOG.md`.
