#!/bin/sh
# One command for the whole demo set: two Riak nodes (the second one as the
# peer cluster of the comparison) are started in Docker from the test image
# libriak-riak:VERSION (built as in .github/workflows/ci.yml), prepared with
# tests/docker/setup-node.sh, run-all.sh runs against them, and the
# containers are removed again.
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

case $transport in
pb) inner=8087 ;;
http) inner=8098 ;;
*) echo "transport pb or http" >&2; exit 2 ;;
esac
name=riakctl-demo-$$
peer=riakctl-demo-peer-$$
trap 'docker rm -f "$name" "$peer" >/dev/null 2>&1' EXIT INT TERM
# the peer gets a node name of its own (Riak 3.x images; the 2.x nodes are
# named after their address), as vnode actor ids derive from it
start() {
    docker run -d --name "$1" --ulimit nofile=65536:65536 -e "RIAK_NODENAME=$2@127.0.0.1" \
        -p 127.0.0.1::8087 -p 127.0.0.1::8098 "$image" >/dev/null
}
start "$name" riak
start "$peer" peer
port() { docker port "$1" "$2/tcp" | head -n 1 | sed 's/.*://'; }
sh "$root/tests/docker/setup-node.sh" "$name"
sh "$root/tests/docker/setup-node.sh" "$peer"

RIAK_URL=$transport://127.0.0.1:$(port "$name" "$inner")
RIAK_PEER_URL=$transport://127.0.0.1:$(port "$peer" "$inner")
RIAK_HTTP_PORT=$(port "$name" 8098)
export RIAK_URL RIAK_PEER_URL RIAK_HTTP_PORT
sh "$root/examples/cli/run-all.sh"
