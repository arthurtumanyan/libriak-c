# Testing

## Test suites

| Test | Contents | Needs |
|---|---|---|
| `util`, `parse`, `multipart`, `datatype`, `api` | Unit tests of buffers, URL encoding, JSON, multipart and data type encoding, argument validation | — |
| `client` | The blocking API against `tests/stub_riak.py` | Python 3 |
| `async` | The asynchronous API, including an external `poll()` loop | Python 3 |
| `tls` | Certificate verification, client certificates, Basic auth | Python 3, OpenSSL |
| `pb` | The Protocol Buffers transport against the in-process stub of `tests/stub_pb.h`: every operation, retries, timeouts, connection reuse, async requests, STARTTLS and authentication | OpenSSL for the TLS part |
| `threads` | Eight threads with one client each, on both transports, every result checked; with `-DRIAK_SANITIZE=thread` ThreadSanitizer checks the library for data races | Python 3 for the HTTP part |
| `docs` | API names used in `README.md` and `docs/` exist; complete example programs build | Python 3 |
| `live` | Everything against a real Riak node | `RIAK_TEST_URL` |

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRIAK_SANITIZE=ON -DRIAK_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests that lack their requirements are reported as skipped.
`-DRIAK_SANITIZE=thread` builds everything with ThreadSanitizer instead of
AddressSanitizer; `tests/tsan.supp` silences only the stub's own request
recorder and the uninstrumented system OpenSSL.

## Stub server

`tests/stub_riak.py` emulates the Riak HTTP API in memory: objects with
metadata and indexes, bucket types (including `allow_mult` siblings),
conditional requests, streamed key lists, secondary indexes, Search,
MapReduce and data types. Three nodes share one store, like a cluster. Each
node can be switched to answer with HTTP 503, to drop connections, or to
answer slowly, and records the last request (method, path, headers, body)
for assertions. In `--tls` mode the nodes speak HTTPS with certificates made
by `tests/gen_certs.sh`. The module docstring lists every control endpoint.

`tests/stub_pb.h` is the Protocol Buffers counterpart: a C server started in
a thread of the test program, built on the same generated message code as
the library. Its replies are chosen by the key or bucket type of a request
(`missing`, `sibs`, `tomb`, `overload`, `drop`, `slow`, `big`, ...), and what
the client sent is recorded in the `stub` structure for assertions. With
`RIAK_TEST_CERTS` set, it answers STARTTLS with the certificates of
`tests/gen_certs.sh` and accepts the user `alice` with the password
`secret`. Server threads outlive the test functions, so LeakSanitizer reads
`tests/lsan.supp`.

## Live tests

The live tests run when `RIAK_TEST_URL` is set. The image in `tests/docker`
enables the leveldb backend (secondary indexes) and Riak Search:

```sh
docker build -t libriak-riak tests/docker
docker run -d --name riak -p 8098:8098 -p 8087:8087 libriak-riak
docker exec riak riak-admin wait-for-service riak_kv
docker exec riak riak-admin bucket-type create libriak_multi '{"props":{"allow_mult":true}}'
docker exec riak riak-admin bucket-type activate libriak_multi
for t in counter set map hll; do
  docker exec riak riak-admin bucket-type create libriak_${t}s '{"props":{"datatype":"'$t'"}}'
  docker exec riak riak-admin bucket-type activate libriak_${t}s
done

RIAK_TEST_URL=http://127.0.0.1:8098 RIAK_TEST_TYPE=libriak_multi \
RIAK_TEST_FEATURES=2i,search,dt ctest --test-dir build -R live --output-on-failure

RIAK_TEST_URL=pb://127.0.0.1:8087 RIAK_TEST_HTTP_PORT=8098 RIAK_TEST_TYPE=libriak_multi \
RIAK_TEST_FEATURES=2i,search,dt ctest --test-dir build -R live --output-on-failure
```

The scheme of `RIAK_TEST_URL` selects the transport: `http` and `https` for
HTTP, `pb` and `pbs` (STARTTLS) for Protocol Buffers. `RIAK_TEST_HTTP_PORT`
gives the HTTP port that `riak_stats()` uses over Protocol Buffers.
Against a node with Riak security enabled, `RIAK_TEST_USER`,
`RIAK_TEST_PASSWORD` and `RIAK_TEST_CA` give the credentials and CA, and
`RIAK_TEST_TLS_LEGACY=1` sets `tls_legacy` (needed for Riak KV 2.2). Such a
node needs `ssl.certfile`, `ssl.keyfile`, `ssl.cacertfile` and
`listener.https.internal` in `riak.conf`, a user with a `password` source
and grants, and a descriptor limit well above 1024 (`docker run --ulimit
nofile=65536:65536`).

`RIAK_TEST_FEATURES` enables the parts that need a prepared node: `2i`,
`search`, `dt` (data type bucket types) and `gset` (Riak 2.9+). The `live`
job in `.github/workflows/ci.yml` performs the same setup and runs the suite
on both transports.

## Fuzzing and coverage

```sh
CC=clang cmake -S . -B build-fuzz -DRIAK_FUZZ=ON -DRIAK_SANITIZE=ON
cmake --build build-fuzz --target fuzz_parse fuzz_pb
build-fuzz/tests/fuzz_parse -max_total_time=300
build-fuzz/tests/fuzz_pb -max_total_time=300
```

`fuzz_parse` covers the JSON parsers, the multipart parser (streamed and
one-shot results are compared), the data type parser and URL decoding.
`fuzz_pb` covers the decoding of Protocol Buffers responses into objects,
sibling sets and put results, the mapping of Riak error messages, and the
base64 decoding of vector clocks and contexts.

```sh
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DRIAK_COVERAGE=ON
cmake --build build-cov && ctest --test-dir build-cov
gcovr --root . --filter src/ --object-directory build-cov build-cov
```

CI fails when line coverage of `src/` drops below 85 %.

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `master` and on pull requests:
GCC and Clang in sanitizer and release builds, ThreadSanitizer, valgrind,
the oldest supported distribution (Ubuntu 22.04), the live job, coverage, a
fuzzing run, a Debian package dry run, and linting (clang-format, cppcheck, clang-tidy, actionlint,
Doxygen). CodeQL runs in `.github/workflows/codeql.yml`.
