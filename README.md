libriak-c
=========

A C11 client library for the [Riak KV](https://github.com/basho/riak) HTTP API,
built on libcurl and json-c.

## Build

Requirements: CMake ≥ 3.20, a C11 compiler, libcurl ≥ 7.62, json-c ≥ 0.15.

```sh
sudo apt install libcurl4-openssl-dev libjson-c-dev   # Debian/Ubuntu
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

Options: `-DRIAK_SANITIZE=ON` (ASan + UBSan), `-DRIAK_WERROR=ON`,
`-DRIAK_COVERAGE=ON`, `-DRIAK_FUZZ=ON` (clang/libFuzzer),
`-DRIAK_BUILD_TESTS=OFF`, `-DRIAK_BUILD_EXAMPLES=OFF`.

### Tests

- `util`, `parse` — unit tests of the internal helpers.
- `client` — the public API end to end against `tests/stub_riak.py`, an
  in-memory emulation of the Riak HTTP API (needs Python 3).
- `live` — against a real node; skipped unless `RIAK_TEST_URL` is set:

  ```sh
  docker run -d -p 8098:8098 basho/riak-kv
  RIAK_TEST_URL=http://127.0.0.1:8098 ctest --test-dir build -R live
  ```
- Fuzzing: `build/tests/fuzz_parse -max_total_time=300` (with `-DRIAK_FUZZ=ON`).

Consumers can use `find_package(libriak-c)` → `riak::riak` / `riak::riak_static`,
or `pkg-config --cflags --libs libriak-c`.

## Usage

```c
#include <riak/riak.h>

riak_global_init();

riak_config cfg;
riak_config_init(&cfg);          /* 127.0.0.1:8098, TLS verification on */
cfg.timeout_ms = 5000;

riak_client *c;
if (riak_client_new(&cfg, &c) != RIAK_OK) { /* ... */ }

riak_opts put = {.content_type = "text/plain", .w = RIAK_Q_QUORUM};
riak_status st = riak_put(c, "test", "doc", "hello", 5, &put, NULL, NULL);
if (st != RIAK_OK)
    fprintf(stderr, "%s: %s\n", riak_strerror(st), riak_client_errmsg(c));

riak_object obj;
if (riak_get(c, "test", "doc", NULL, &obj) == RIAK_OK) {
    printf("%.*s (vclock %s)\n", (int)obj.len, obj.data, obj.vclock);
    riak_object_free(&obj);
}

riak_strlist keys;
if (riak_list_keys(c, "test", &keys) == RIAK_OK) {
    for (char **k = keys.items; *k; k++) puts(*k);
    riak_strlist_free(&keys);
}

riak_client_free(c);
riak_global_cleanup();
```

A complete program is in [`examples/basic.c`](examples/basic.c).

### Rules

- Every function returns a `riak_status`; `riak_client_errmsg()` gives detail
  and `riak_client_http_status()` the last HTTP status.
- Anything returned through an out-parameter belongs to the caller: free it
  with `riak_object_free()`, `riak_strlist_free()` or `riak_free()`.
- String lists are NULL-terminated and also carry `count`.
- Bucket and key names are percent-encoded for you; header values containing
  CR/LF are rejected with `RIAK_E_INVALID_ARG`.
- `riak_put()` with `key == NULL` lets Riak choose a key and returns it
  through `created_key`.
- Call `riak_global_init()` once before starting threads. A client is not
  thread-safe; use one client per thread.
- TLS certificates are verified by default (`tls_verify`, `ca_file`).
- Responses larger than `max_body_bytes` (default 64 MiB) are rejected.

## Migrating from 1.x

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

See [CHANGELOG.md](CHANGELOG.md) for the bugs fixed in 2.0.

## License

GPL-3.0-or-later.
