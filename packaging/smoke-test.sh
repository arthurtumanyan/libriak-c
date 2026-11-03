#!/bin/sh
# Compile and link examples/basic.c against an installed libriak-c, once via
# pkg-config and once via the CMake package, and check riakctl when the tools
# package is installed. Run inside a clean system that has the runtime and
# development packages installed.
set -eu
src=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -Wall -Werror -o "$work/basic-pc" "$src/examples/basic.c" \
    $(pkg-config --cflags --libs libriak-c)
"$work/basic-pc" 127.0.0.1 1 >/dev/null 2>&1 && exit 1  # nothing listens: must fail cleanly
ldd "$work/basic-pc" | grep -q 'libriak-c\.so\.2'

mkdir "$work/cm"
cat >"$work/cm/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.20)
project(consumer C)
find_package(libriak-c 2 REQUIRED)
add_executable(basic $src/examples/basic.c)
target_link_libraries(basic PRIVATE riak::riak)
add_executable(basic_static $src/examples/basic.c)
target_link_libraries(basic_static PRIVATE riak::riak_static)
CMAKE
cmake -S "$work/cm" -B "$work/cm/b" >/dev/null
cmake --build "$work/cm/b" >/dev/null
if command -v riakctl >/dev/null 2>&1; then
    riakctl --version | grep -q "^riakctl (libriak-c) "
    ldd "$(command -v riakctl)" | grep -q 'libriak-c\.so\.2'
    rc=0
    riakctl -u pb://127.0.0.1:1 --connect-timeout 1000 ping >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 7 ]  # nothing listens: "unreachable"
    riakctl help backup >/dev/null
    # the package lists the man page (minimal images may skip installing it)
    { dpkg -L riakctl 2>/dev/null || rpm -ql riakctl 2>/dev/null; } |
        grep -q 'man1/riakctl\.1'
    echo "riakctl smoke test passed"
fi
echo "smoke test passed"
