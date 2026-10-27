# Packaging and releases

## Packages

Each format consists of a runtime and a development package:

| | Debian / Ubuntu | Fedora / EL |
|---|---|---|
| Runtime | `libriak-c2` | `libriak-c` |
| Development | `libriak-c-dev` | `libriak-c-devel` |

The runtime package holds the shared library and documentation; the
development package holds the header, the static library, the `.so` link,
the CMake package and the pkg-config file. Runtime dependencies are derived
by `dpkg-shlibdeps` and RPM autoreq; the development packages depend on the
libcurl and json-c development packages.

Package versions carry the distribution (`2.0.0-1~ubuntu24.04`,
`2.0.0-1.fc44`), so packages for different distributions do not collide.

Packages are built locally with:

```sh
packaging/build-packages.sh DEB      # or RPM; output in dist/
```

`packaging/smoke-test.sh` compiles `examples/basic.c` against the installed
packages through pkg-config and through the CMake package, shared and static.

## Releases

A release is made by updating `project(VERSION ...)` in `CMakeLists.txt`,
the version macros in `include/riak/riak.h`, `PROJECT_NUMBER` in
`docs/Doxyfile` and the `CHANGELOG.md` heading, and pushing a matching tag:

```sh
git tag -s v2.0.0 -m "libriak-c 2.0.0"
git push origin v2.0.0
```

`.github/workflows/release.yml` then checks that the tag matches the project
version, builds and install-tests packages on Ubuntu 22.04 and 24.04, Fedora
and AlmaLinux 10, and publishes them with a source tarball, `SHA256SUMS` and
build provenance attestations. The release notes are taken from the
`CHANGELOG.md` section of the version.
