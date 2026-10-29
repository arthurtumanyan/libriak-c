# libriak-c documentation {#mainpage}

libriak-c is a C11 client library for Riak KV. It speaks Riak's Protocol
Buffers interface (the default) and its HTTP interface, and is built on
protobuf-c, OpenSSL, libcurl and json-c.

## Guides

| Guide | Contents |
|---|---|
| [Getting started](getting-started.md) | Building, installing, linking, a first program |
| [Configuration](configuration.md) | Every `riak_config` field and its default |
| [Transports](transports.md) | Protocol Buffers and HTTP, their differences, connections, coverage plans |
| [Errors and ownership](errors.md) | Status codes, error details, memory ownership, threading |
| [Features](features.md) | Which call each transport and Riak version serves |
| [Objects and buckets](objects.md) | Get, put, delete, metadata, bucket types, siblings, conditional requests, legacy counters |
| [Queries](queries.md) | Secondary indexes, Query API, Riak Search, MapReduce, object folds, link walking |
| [Operations](operations.md) | AAE folds, replication queues, riak_repl, known server issues |
| [Data types](datatypes.md) | Counters, sets, maps, HyperLogLogs, grow-only sets |
| [Clusters and retries](clusters.md) | Multiple nodes, balancing, retries, node ejection |
| [Asynchronous requests](async.md) | Concurrent requests, built-in and external event loops |
| [Security](security.md) | TLS, STARTTLS, certificate verification, authentication, client certificates |
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

The library is tested against Riak KV 2.0.7, 2.1.4, 2.2.0 and 2.2.3 and the
OpenRiak releases 3.2.6 and 3.4.1, on both transports, with and without TLS
and authentication.
Calls that a node does not serve (Riak Search on 3.x, the AAE folds and
replication queues on 2.2, the Query API before 3.4) return
`RIAK_E_UNSUPPORTED`; [Features](features.md) has the full matrix.
