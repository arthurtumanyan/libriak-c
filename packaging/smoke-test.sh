#!/bin/sh
# Compile and link examples/basic.c against an installed libriak-c, once via
# pkg-config and once via the CMake package. Run inside a clean system that
# has the runtime and development packages installed.
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
echo "smoke test passed"
