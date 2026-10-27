# libriak-c documentation {#mainpage}

libriak-c is a C11 client library for the Riak KV HTTP API, built on libcurl
and json-c.

## Guides

| Guide | Contents |
|---|---|
| [Getting started](getting-started.md) | Building, installing, linking, a first program |
| [Configuration](configuration.md) | Every `riak_config` field and its default |
| [Errors and ownership](errors.md) | Status codes, error details, memory ownership, threading |
| [Objects and buckets](objects.md) | Get, put, delete, metadata, bucket types, siblings, conditional requests |
| [Queries](queries.md) | Secondary indexes, Riak Search, MapReduce |
| [Data types](datatypes.md) | Counters, sets, maps, HyperLogLogs, grow-only sets |
| [Clusters and retries](clusters.md) | Multiple nodes, balancing, retries, node ejection |
| [Asynchronous requests](async.md) | Concurrent requests, built-in and external event loops |
| [Security](security.md) | TLS, certificate verification, Basic auth, client certificates |
| [Logging and metrics](observability.md) | Log and metrics callbacks |
| [Testing](testing.md) | Test suites, stub server, live tests, fuzzing, coverage |
| [Packaging and releases](packaging.md) | Debian/RPM packages and the release workflow |
| [Migrating from 1.x](migration.md) | Mapping of the 1.x functions to the 2.0 API |

## API reference

The reference is generated from `include/riak/riak.h` with Doxygen:

```sh
cmake --build build --target docs
```

The HTML pages are written to `build/docs/html/`. Without CMake, the
reference is generated with
`mkdir -p out && RIAK_DOCS_OUT=out doxygen docs/Doxyfile`. Every public type,
field and function is documented; the build fails on undocumented symbols.

## Supported servers

The library is tested against Riak KV 2.2.3. Riak KV 2.9+ and the
community-maintained OpenRiak releases expose the same HTTP API; MapReduce
is not available on OpenRiak 4.x, and grow-only sets need Riak 2.9 or newer.
