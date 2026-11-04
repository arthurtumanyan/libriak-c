#!/bin/sh
# Submit a release to the Copr project: packaging/copr-build.sh VERSION [PROJECT]
# Copr fetches the spec of the release tag and its Source0 (the release
# tarball). Needs copr-cli with an API token in ~/.config/copr.
set -eu
version=${1:?release version, e.g. 2.2.2}
project=${2:-atumanyan/libriak-c}
spec=https://raw.githubusercontent.com/arthurtumanyan/libriak-c/v$version/packaging/rpm/libriak-c.spec
curl -fsS "$spec" | grep -q "^Version: *$version\$" ||
    { echo "spec of v$version does not carry version $version" >&2; exit 1; }
exec copr-cli build "$project" "$spec"
