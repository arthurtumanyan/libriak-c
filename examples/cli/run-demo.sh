#!/bin/sh
# One command for the whole demo set: a Riak node is started in Docker from
# the test image libriak-riak:VERSION (built as in .github/workflows/ci.yml),
# prepared with tests/docker/setup-node.sh, run-all.sh runs against it, and
# the container is removed again.
#   examples/cli/run-demo.sh 3.4.1 [pb|http]
# riakctl is taken from build/tools/riakctl when it was built there, from
# PATH otherwise (RIAKCTL overrides both).
set -eu
version=${1:?Riak version of the image, e.g. 3.4.1}
transport=${2:-pb}
root=$(cd "$(dirname "$0")/../.." && pwd)
image=libriak-riak:$version
if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "image $image not found; it is built as in .github/workflows/ci.yml" >&2
    exit 2
fi
if [ -z "${RIAKCTL:-}" ] && [ -x "$root/build/tools/riakctl/riakctl" ]; then
    RIAKCTL=$root/build/tools/riakctl/riakctl
fi
export RIAKCTL="${RIAKCTL:-riakctl}"

name=riakctl-demo-$$
trap 'docker rm -f "$name" >/dev/null 2>&1' EXIT INT TERM
docker run -d --name "$name" --ulimit nofile=65536:65536 \
    -p 127.0.0.1::8087 -p 127.0.0.1::8098 "$image" >/dev/null
port() { docker port "$name" "$1/tcp" | head -n 1 | sed 's/.*://'; }
pb=$(port 8087)
http=$(port 8098)
sh "$root/tests/docker/setup-node.sh" "$name"

case $transport in
pb) RIAK_URL=pb://127.0.0.1:$pb ;;
http) RIAK_URL=http://127.0.0.1:$http ;;
*) echo "transport pb or http" >&2; exit 2 ;;
esac
export RIAK_URL RIAK_HTTP_PORT="$http"
sh "$root/examples/cli/run-all.sh"
