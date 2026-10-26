# Changelog

## Unreleased

### Added
- Bucket types: `riak_opts.bucket_type` for object calls and `riak_type_*`
  variants of the bucket calls; `riak_get_type_props()`.
- Siblings: `riak_get_siblings()` parses `multipart/mixed` responses into a
  `riak_object_set`; `riak_opts.resolver` lets `riak_get()` pick one sibling,
  returned with the vclock that resolves the conflict on write-back.
- Conditional requests: `if_none_match`, `if_match`, `if_modified_since`,
  `if_unmodified_since`; new status `RIAK_E_NOT_MODIFIED` for 304.
- `riak_object` now carries user metadata, secondary index entries, links and
  a tombstone flag.
- `riak_opts.server_timeout_ms` (Riak `timeout` parameter) and `riak_stats()`.
- Debian and RPM packages via CPack (runtime and development components),
  `packaging/build-packages.sh` and a package smoke test.
- Release workflow: builds and install-tests packages on Ubuntu 22.04/24.04,
  Fedora and AlmaLinux 10, publishes them with a source tarball, checksums and
  build provenance attestations.
- CI: least-privilege permissions, actions pinned to commit SHAs, jobs for
  the oldest supported distribution, a live Riak node, coverage (fails under
  85 % lines), a fuzzing smoke run, a package dry run and actionlint; CodeQL
  analysis and Dependabot updates for actions.
- `SECURITY.md`, `CONTRIBUTING.md`, issue and pull request templates,
  `CODEOWNERS`.

## 2.0.0 — 2026-10-26

Complete rewrite. The public API is new and not source-compatible with 1.x;
see the migration table in the README.

### Fixed
- Library failed to link with GCC ≥ 10 (`-fno-common`): global variables were
  defined in the public header.
- A request method, body or header list set by one call stayed on the curl
  handle: after `RiakDeleteKey()` the next `RiakGetKey()` sent `DELETE`, and
  the header list pointed at freed memory.
- Heap overflow building the server URL (fixed 30-byte buffer).
- Heap overflow in bucket/key list parsing (`malloc(strlen(s))`).
- Off-by-one writes past every header buffer; `X-Riak-ClientId` header
  overflowed its buffer.
- Key/bucket lists were not NULL-terminated although the documented usage
  iterated until NULL.
- JSON was parsed on every partial network chunk (crash on split responses,
  leaks); streamed key listing (`keys=stream`) now works.
- NULL dereference when `content_type`, `metadata` or `index_entry` was unset.
- Ping read a non-terminated buffer and an uninitialised status.
- PUT without `returnbody` (HTTP 204) was reported as a failure.
- Auto-generated keys were truncated in the URL and never returned.
- Bucket and key names were not URL-encoded; header values were not checked
  for CR/LF injection.
- Quorum options (`r`, `w`, `dw`, `pr`, `pw`, `rw`) were accepted but never
  sent.
- Leaks: per-call response buffers, `/dev/null` file handle, server URL,
  header lists on early returns.

### Security
- TLS peer and host verification is now enabled by default
  (it was unconditionally disabled).

### Added
- `riak_get_bucket_props()` / `riak_set_bucket_props()` (were unimplemented).
- Status codes (`riak_status`) with per-client error messages.
- Response metadata: content type, vclock, ETag, Last-Modified.
- Timeouts, CA bundle, client-id and response size limit (`max_body_bytes`)
  configuration.
- CMake build with install/export, pkg-config file, unit and integration
  tests (stub Riak server and optional live Riak node), libFuzzer target,
  coverage and sanitizer builds, GitHub Actions CI.

### Removed
- NetBeans project files and committed prebuilt `.so` binaries.
