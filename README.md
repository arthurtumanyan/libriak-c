libriak-c
=========

A C11 client library for [Riak KV](https://github.com/basho/riak) that speaks
both of Riak's interfaces: Protocol Buffers (the default) and HTTP. It is
built on protobuf-c, OpenSSL, libcurl and json-c.

## Features

- Two transports with the same API: Protocol Buffers (port 8087, persistent
  connections, about 2–2.5× the throughput of HTTP) and HTTP (port 8098)
- Objects: get, put, delete, metadata, secondary index entries, links,
  bucket types, sibling resolution, conditional requests
- Queries: secondary indexes (paged, streamed, split by coverage plans),
  Riak Search, MapReduce
- Streamed key and bucket listings, preference lists, server information
- Data types: counters, sets, maps, HyperLogLogs, grow-only sets
- Clusters: round-robin or failover node selection, retries with backoff,
  ejection of failing nodes
- Asynchronous requests on one thread, with hooks for external event loops
- Security: verified TLS (HTTPS or STARTTLS), password authentication,
  client certificates
- Logging and per-request metrics callbacks
- Debian and RPM packages, a CMake package and a pkg-config file

## Building

```sh
sudo apt install cmake libcurl4-openssl-dev libjson-c-dev \
    libprotobuf-c-dev protobuf-c-compiler libssl-dev           # Debian/Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

Requirements: CMake ≥ 3.20, a C11 compiler, libcurl ≥ 7.66, json-c ≥ 0.15,
protobuf-c ≥ 1.3 (with `protoc-c`), OpenSSL ≥ 1.1.1.
Prebuilt packages are attached to the
[releases](https://github.com/arthurtumanyan/libriak-c/releases).

## Example

```c
#include <stdio.h>
#include <riak/riak.h>

int main(void) {
    riak_global_init();
    riak_config cfg;
    riak_config_init(&cfg);                 /* Protocol Buffers, 127.0.0.1:8087 */
    riak_client *c;
    if (riak_client_new(&cfg, &c) != RIAK_OK)
        return 1;

    riak_opts o = {.content_type = "text/plain"};
    riak_put(c, "greetings", "hello", "world", 5, &o, NULL, NULL);

    riak_object obj;
    riak_status st = riak_get(c, "greetings", "hello", NULL, &obj);
    if (st == RIAK_OK) {
        printf("%.*s\n", (int)obj.len, obj.data);
        riak_object_free(&obj);
    } else {
        fprintf(stderr, "%s: %s\n", riak_strerror(st), riak_client_errmsg(c));
    }

    riak_client_free(c);
    riak_global_cleanup();
    return 0;
}
```

The program is linked with `pkg-config --cflags --libs libriak-c` or, in
CMake, with `find_package(libriak-c)` and the `riak::riak` target.

## Documentation

The guides are found in [`docs/`](docs/index.md):

- [Getting started](docs/getting-started.md)
- [Configuration](docs/configuration.md)
- [Transports](docs/transports.md)
- [Errors and ownership](docs/errors.md)
- [Objects and buckets](docs/objects.md)
- [Queries](docs/queries.md)
- [Data types](docs/datatypes.md)
- [Clusters and retries](docs/clusters.md)
- [Asynchronous requests](docs/async.md)
- [Security](docs/security.md)
- [Logging and metrics](docs/observability.md)
- [Testing](docs/testing.md)
- [Packaging and releases](docs/packaging.md)
- [Migrating from 1.x](docs/migration.md)

The API reference is generated from `include/riak/riak.h` with
`cmake --build build --target docs` (Doxygen). Changes between versions are
listed in [CHANGELOG.md](CHANGELOG.md).

## Contributing

Contribution rules are described in [CONTRIBUTING.md](CONTRIBUTING.md);
security issues are reported as described in [SECURITY.md](SECURITY.md).

## License

GPL-3.0-or-later.
