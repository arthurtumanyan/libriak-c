#!/bin/sh
# Signed source packages for the Launchpad PPA, one per Ubuntu series:
#   packaging/ppa-build.sh VERSION KEY OUTDIR SERIES...
# The orig tarball is the release tarball of VERSION; debian/ comes from
# packaging/debian of this tree. Needs dpkg-dev, debhelper, devscripts and curl.
set -eu
version=${1:?version}; key=${2:?signing key}; out=${3:?output directory}; shift 3
[ $# -gt 0 ] || { echo "no Ubuntu series given" >&2; exit 2; }
src=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"; out=$(cd "$out" && pwd)
orig=$out/libriak-c_$version.orig.tar.gz
[ -f "$orig" ] || curl -fsSL -o "$orig" \
    "https://github.com/arthurtumanyan/libriak-c/releases/download/v$version/libriak-c-$version.tar.gz"
for series in "$@"; do
    work=$(mktemp -d)
    cp "$orig" "$work/"
    tar xzf "$orig" -C "$work"
    cp -r "$src/packaging/debian" "$work/libriak-c-$version/debian"
    # one upload per series; the suffix keeps the versions ordered by release
    cat > "$work/libriak-c-$version/debian/changelog" <<EOT
libriak-c ($version-1~${series}1) $series; urgency=medium

  * Release $version.

 -- Arthur Tumanyan <arthurtumanyan@gmail.com>  $(date -R)
EOT
    (cd "$work/libriak-c-$version" && dpkg-buildpackage -S -d -sa -us -uc >/dev/null)
    debsign -k"$key" "$work"/*.changes >/dev/null
    cp "$work"/*.dsc "$work"/*.debian.tar.* "$work"/*_source.changes "$work"/*_source.buildinfo "$out/"
    rm -rf "$work"
done
ls "$out"
