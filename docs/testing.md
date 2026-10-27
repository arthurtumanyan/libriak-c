# Testing

## Test suites

| Test | Contents | Needs |
|---|---|---|
| `util`, `parse`, `multipart`, `datatype`, `api` | Unit tests of buffers, URL encoding, JSON, multipart and data type encoding, argument validation | — |
| `client` | The blocking API against `tests/stub_riak.py` | Python 3 |
| `async` | The asynchronous API, including an external `poll()` loop | Python 3 |
| `tls` | Certificate verification, client certificates, Basic auth | Python 3, OpenSSL |
| `docs` | API names used in `README.md` and `docs/` exist; complete example programs build | Python 3 |
| `live` | Everything against a real Riak node | `RIAK_TEST_URL` |

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRIAK_SANITIZE=ON -DRIAK_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests that lack their requirements are reported as skipped.

## Stub server

`tests/stub_riak.py` emulates the Riak HTTP API in memory: objects with
metadata and indexes, bucket types (including `allow_mult` siblings),
conditional requests, streamed key lists, secondary indexes, Search,
MapReduce and data types. Three nodes share one store, like a cluster. Each
node can be switched to answer with HTTP 503, to drop connections, or to
answer slowly, and records the last request (method, path, headers, body)
for assertions. In `--tls` mode the nodes speak HTTPS with certificates made
by `tests/gen_certs.sh`. The module docstring lists every control endpoint.

## Live tests

The live tests run when `RIAK_TEST_URL` is set. The image in `tests/docker`
enables the leveldb backend (secondary indexes) and Riak Search:

```sh
docker build -t libriak-riak tests/docker
docker run -d --name riak -p 8098:8098 libriak-riak
docker exec riak riak-admin wait-for-service riak_kv
docker exec riak riak-admin bucket-type create libriak_multi '{"props":{"allow_mult":true}}'
docker exec riak riak-admin bucket-type activate libriak_multi
for t in counter set map hll; do
  docker exec riak riak-admin bucket-type create libriak_${t}s '{"props":{"datatype":"'$t'"}}'
  docker exec riak riak-admin bucket-type activate libriak_${t}s
done

RIAK_TEST_URL=http://127.0.0.1:8098 RIAK_TEST_TYPE=libriak_multi \
RIAK_TEST_FEATURES=2i,search,dt ctest --test-dir build -R live --output-on-failure
```

`RIAK_TEST_FEATURES` enables the parts that need a prepared node: `2i`,
`search`, `dt` (data type bucket types) and `gset` (Riak 2.9+). The `live`
job in `.github/workflows/ci.yml` performs the same setup.

## Fuzzing and coverage

```sh
CC=clang cmake -S . -B build-fuzz -DRIAK_FUZZ=ON -DRIAK_SANITIZE=ON
cmake --build build-fuzz --target fuzz_parse
build-fuzz/tests/fuzz_parse -max_total_time=300
```

The fuzz target covers the JSON parsers, the multipart parser (streamed and
one-shot results are compared), the data type parser and URL decoding.

```sh
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DRIAK_COVERAGE=ON
cmake --build build-cov && ctest --test-dir build-cov
gcovr --root . --filter src/ --object-directory build-cov build-cov
```

CI fails when line coverage of `src/` drops below 85 %.

## Continuous integration

`.github/workflows/ci.yml` runs on pushes to `master` and on pull requests:
GCC and Clang in sanitizer and release builds, valgrind, the oldest supported
distribution (Ubuntu 22.04), the live job, coverage, a fuzzing run, a Debian
package dry run, and linting (clang-format, cppcheck, clang-tidy, actionlint,
Doxygen). CodeQL runs in `.github/workflows/codeql.yml`.
