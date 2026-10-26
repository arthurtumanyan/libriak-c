#!/bin/sh
# Build release packages into ./dist: packaging/build-packages.sh DEB|RPM
set -eu
gen=${1:?generator DEB or RPM}
src=$(cd "$(dirname "$0")/.." && pwd)
build=$(mktemp -d)
suffix=
if [ "$gen" = DEB ] && [ -r /etc/os-release ]; then
    . /etc/os-release
    suffix="~${ID}${VERSION_ID}"
fi
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DRIAK_BUILD_TESTS=OFF -DRIAK_BUILD_EXAMPLES=OFF -DRIAK_DEB_DIST_SUFFIX="$suffix"
cmake --build "$build" -j
(cd "$build" && cpack -G "$gen")
mkdir -p "$src/dist"
cp "$build"/*.deb "$build"/*.rpm "$src/dist/" 2>/dev/null || true
rm -rf "$build"
ls -l "$src/dist"
