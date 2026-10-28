# Getting started

## Requirements

- CMake 3.20 or newer and a C11 compiler (GCC or Clang)
- libcurl 7.66 or newer
- json-c 0.15 or newer
- protobuf-c 1.3 or newer, with the `protoc-c` compiler
- OpenSSL 1.1.1 or newer
- Python 3 and OpenSSL for the test suite (optional)

On Debian and Ubuntu the dependencies are installed with:

```sh
sudo apt install cmake libcurl4-openssl-dev libjson-c-dev libprotobuf-c-dev \
    protobuf-c-compiler libssl-dev
```

On Fedora and EL:

```sh
sudo dnf install cmake gcc libcurl-devel json-c-devel protobuf-c-devel \
    protobuf-c-compiler openssl-devel
```

## Building and installing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The shared library (`libriak-c.so.2`), the static library, the header
`riak/riak.h`, a CMake package and a pkg-config file are installed.

Build options:

| Option | Default | Effect |
|---|---|---|
| `RIAK_BUILD_TESTS` | ON (top level) | Test suite |
| `RIAK_BUILD_EXAMPLES` | ON (top level) | `examples/basic.c`, `examples/async.c` |
| `RIAK_SANITIZE` | OFF | AddressSanitizer and UndefinedBehaviorSanitizer |
| `RIAK_WERROR` | OFF | Warnings are treated as errors |
| `RIAK_COVERAGE` | OFF | gcov instrumentation |
| `RIAK_FUZZ` | OFF | libFuzzer targets (Clang only) |

Prebuilt `.deb` and `.rpm` packages are attached to the GitHub releases, see
[Packaging and releases](packaging.md).

## Linking

With CMake:

```cmake
find_package(libriak-c 2 REQUIRED)
target_link_libraries(app PRIVATE riak::riak)        # or riak::riak_static
```

With pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs libriak-c)
```

## A first program

```c
#include <stdio.h>
#include <riak/riak.h>

int main(void) {
    riak_global_init();

    riak_config cfg;
    riak_config_init(&cfg);              /* Protocol Buffers, 127.0.0.1:8087 */
    riak_client *c;
    if (riak_client_new(&cfg, &c) != RIAK_OK)
        return 1;

    riak_opts put = {.content_type = "text/plain"};
    riak_status st = riak_put(c, "greetings", "hello", "world", 5, &put, NULL, NULL);
    if (st != RIAK_OK)
        fprintf(stderr, "put: %s (%s)\n", riak_strerror(st), riak_client_errmsg(c));

    riak_object obj;
    if (riak_get(c, "greetings", "hello", NULL, &obj) == RIAK_OK) {
        printf("%.*s\n", (int)obj.len, obj.data);
        riak_object_free(&obj);
    }

    riak_client_free(c);
    riak_global_cleanup();
    return 0;
}
```

A local Riak node for experiments is started with Docker:

```sh
docker run -d -p 8087:8087 -p 8098:8098 basho/riak-kv
```

Complete programs are found in `examples/basic.c` and `examples/async.c`.
The transports and their differences are described in
[Transports](transports.md).
