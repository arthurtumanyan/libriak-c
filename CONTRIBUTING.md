# Contributing

## Build and test

```sh
sudo apt install libcurl4-openssl-dev libjson-c-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRIAK_SANITIZE=ON -DRIAK_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Live tests run against a real node when `RIAK_TEST_URL` is set:

```sh
docker run -d -p 8098:8098 basho/riak-kv
RIAK_TEST_URL=http://127.0.0.1:8098 ctest --test-dir build -R live
```

## Rules

- C11, no compiler extensions; the build must be warning-free with
  `-DRIAK_WERROR=ON` on gcc and clang.
- Format with `clang-format` (version pinned in `.github/workflows/ci.yml`).
- Every change to behaviour comes with a test: unit tests for pure helpers,
  `tests/stub_riak.py` + `tests/test_client.c` for HTTP behaviour.
- Public API: every out-parameter is caller-owned with a matching free
  function; every function returns `riak_status`.
- Keep line coverage of `src/` at or above 85 % (CI enforces it).
- Commit messages: imperative subject of at most 72 characters, a body
  explaining why.
