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

The stub tests check what the client sends (every request field, path and
query parameter, and the decoded filter and query documents), not only what
it returns. Requests that change state (queue fetches and pushes, counter
updates, AAE reaps, erasures, repairs and replication) are checked to be
sent once when the connection drops before the answer, while reads are
retried. Malformed server documents are checked to give `RIAK_E_PARSE`.

## Stub server

`tests/stub_riak.py` emulates the Riak HTTP API in memory: objects with
metadata and indexes, bucket types (including `allow_mult` siblings),
conditional requests, streamed key lists, secondary indexes, Search,
MapReduce and data types. Three nodes share one store, like a cluster. Each
node can be switched to answer with HTTP 503, to drop connections, or to
answer slowly, to answer like Riak 2.2 (`/_old`) or Riak 3.x (`/_v3`), to
answer the newer endpoints with malformed documents (`/_malformed`), to
drop the next requests to them without an answer (`/_drop`), and records
the last request (method, path, headers, body, decoded AAE filter and Query
API document) for assertions. In `--tls` mode the nodes speak HTTPS with certificates made
by `tests/gen_certs.sh`. The module docstring lists every control endpoint.

`tests/stub_pb.h` is the Protocol Buffers counterpart: a C server started in
a thread of the test program, built on the same generated message code as
the library. Its replies are chosen by the key or bucket type of a request
(`missing`, `sibs`, `tomb`, `overload`, `drop`, `slow`, `big`, ...), and what
the client sent is recorded in the `stub` and `ext` structures for
assertions. `ext.drop_code` closes the connection on the next request with
a given message code, and `ext.old_node` answers newer requests like
Riak 2.2. With
`RIAK_TEST_CERTS` set, it answers STARTTLS with the certificates of
`tests/gen_certs.sh` and accepts the user `alice` with the password
`secret`. Server threads outlive the test functions, so LeakSanitizer reads
`tests/lsan.supp`.

## Live tests

The live tests run when `RIAK_TEST_URL` is set. Three node images are used:

| Image | Riak KV | Built from |
|---|---|---|
| `tests/docker` | 2.2.3 | The basho packages; leveldb backend, Riak Search |
| `tests/docker/openriak` | 3.4.1 (OTP 26) | The OpenRiak sources (`make rel`) |
| `tests/docker/openriak` with `RIAK_TAG=riak-3.2.6`, `OTP=24` | 3.2.6 | The OpenRiak sources |

The OpenRiak images use the leveled backend (secondary indexes), tictac AAE
(AAE folds) and a nextgenrepl source queue `libriak_q`; building one takes
about 15 minutes. `tests/docker/setup-node.sh` waits for the node and
creates the bucket types the tests use; with `--security` it also installs
the certificates of `tests/gen_certs.sh`, enables Riak security and adds the
user `alice` (password `secret`). It works with `riak-admin` (2.x) and
`riak admin` (3.x).

```sh
docker build -t libriak-riak:3.4.1 tests/docker/openriak
docker run -d --name riak --ulimit nofile=65536:65536 \
    -p 8098:8098 -p 8087:8087 -p 8443:8443 libriak-riak:3.4.1
sh tests/docker/setup-node.sh riak

F=2i,dt,gset,counters,fold,linkwalk,aae,nextgenrepl,repl,nohttppreflist,query
RIAK_TEST_URL=http://127.0.0.1:8098 RIAK_TEST_TYPE=libriak_multi RIAK_TEST_FEATURES=$F \
    ctest --test-dir build -R live --output-on-failure
RIAK_TEST_URL=pb://127.0.0.1:8087 RIAK_TEST_HTTP_PORT=8098 RIAK_TEST_TYPE=libriak_multi \
    RIAK_TEST_FEATURES=$F ctest --test-dir build -R live --output-on-failure
```

The scheme of `RIAK_TEST_URL` selects the transport: `http` and `https` for
HTTP, `pb` and `pbs` (STARTTLS) for Protocol Buffers. `RIAK_TEST_HTTP_PORT`
gives the HTTP port that `riak_stats()` and the Query API use over Protocol
Buffers. Against a node set up with `--security`, `RIAK_TEST_USER`,
`RIAK_TEST_PASSWORD` and `RIAK_TEST_CA` give the credentials and CA, and
`RIAK_TEST_TLS_LEGACY=1` sets `tls_legacy` (needed for Riak KV 2.2 only).
`RIAK_TEST_LOG=1` prints the library's warnings. Nodes need a descriptor
limit well above 1024 (`--ulimit nofile=65536:65536`).

`RIAK_TEST_FEATURES` enables the parts that the node supports:

| Flag | Meaning | 2.2.3 | 3.2.6 | 3.4.1 |
|---|---|---|---|---|
| `2i` | Secondary indexes | yes | yes | yes |
| `search` | Riak Search | yes | | |
| `dt` | Data type bucket types | yes | yes | yes |
| `gset` | Grow-only sets | | yes | yes |
| `counters` | Legacy counters | yes | yes | yes |
| `fold` | Object folds | yes | yes | yes |
| `linkwalk` | Link walking (not under security) | yes | yes | yes |
| `aae` | AAE folds | | yes | yes |
| `nextgenrepl` | Replication queues, membership | | yes | yes |
| `repl` | `riak_get_cluster_id()`, `riak_repl_get()` | | yes | yes |
| `nohttppreflist` | Preference lists are PB only | | yes | yes |
| `query` | Query API | | | yes |

Features that are not enabled are checked to return `RIAK_E_UNSUPPORTED`
where the node lacks them. The `live` job in `.github/workflows/ci.yml`
runs every version, plain and with TLS and authentication, on both
transports; the OpenRiak images are cached between runs.

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
the oldest supported distribution (Ubuntu 22.04), the live matrix, coverage, a fuzzing run, a Debian
package dry run, and linting (clang-format, cppcheck, clang-tidy, actionlint,
Doxygen). CodeQL runs in `.github/workflows/codeql.yml`.
