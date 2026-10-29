# Contributing

## Building and testing

A development build with sanitizers and warnings treated as errors is made
with:

```sh
sudo apt install cmake libcurl4-openssl-dev libjson-c-dev python3 openssl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRIAK_SANITIZE=ON -DRIAK_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The live tests, fuzzing and coverage are described in
[docs/testing.md](docs/testing.md).

## Rules

- Code is written in C11 without compiler extensions and builds without
  warnings on GCC and Clang with `-DRIAK_WERROR=ON`.
- Code is formatted with `clang-format`, using the version pinned in
  `.github/workflows/ci.yml`; `clang-tidy` and `cppcheck` report no findings.
- Every change in behaviour is covered by a test: unit tests for internal
  helpers, `tests/stub_riak.py` with `tests/test_client.c` or
  `tests/test_async.c` for HTTP behaviour, and `tests/test_live.c` where a
  real node behaves differently from the stub.
- Line coverage of `src/` stays at or above 85 % (enforced by CI).
- Public API: every out-parameter is owned by the caller and has a matching
  free function, every function returns `riak_status`, and every public
  symbol carries a Doxygen comment (`docs/Doxyfile` fails on undocumented
  symbols).
- User-visible changes are recorded under "Unreleased" in `CHANGELOG.md`, and
  the guides in `docs/` are updated with them.
- Binary compatibility within a major version: public structures that
  programs allocate (`riak_config`, `riak_opts`, ...) are never changed in
  place. A changed structure gets a new symbol version node in
  `cmake/libriak-c.map`, a frozen copy of its previous layout and entry
  points for it (as `src/compat_v2.c` does for 2.0), and the functions that
  take it get the new version as their default. Structures the library
  allocates (`riak_async_result`, `riak_request_metrics`) only grow at the
  end. Names are never moved between version nodes; `abi_symbols` and the
  `abi` CI job check it.

## Writing style

Documentation, comments, commit messages and pull request descriptions are
written in an impersonal style, preferably in passive voice ("The client is
created with...", "Requests are retried..."). First and second person and
promotional wording are avoided.

## Commits and pull requests

Commit subjects are short descriptive summaries of at most 72 characters
("Retry on 503 for idempotent requests"); the body, written in passive voice,
explains why the change is made. Pull requests are merged after every CI
check has passed.
