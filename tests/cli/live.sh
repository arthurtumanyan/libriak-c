#!/bin/sh
# ctest riakctl_demos: examples/cli/run-all.sh against the node of the live
# tests (RIAK_TEST_URL and friends, see docs/testing.md).
#   tests/cli/live.sh RIAKCTL
set -eu
if [ -z "${RIAK_TEST_URL:-}" ]; then
    echo "RIAK_TEST_URL not set; skipping the riakctl demos" >&2
    exit 77
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq not found; skipping the riakctl demos" >&2
    exit 77
fi
RIAKCTL=${1:?riakctl binary}
export RIAKCTL
export RIAK_URL="$RIAK_TEST_URL"
[ -n "${RIAK_TEST_HTTP_PORT:-}" ] && export RIAK_HTTP_PORT="$RIAK_TEST_HTTP_PORT"
[ -n "${RIAK_TEST_USER:-}" ] && export RIAK_USER="$RIAK_TEST_USER"
[ -n "${RIAK_TEST_PASSWORD:-}" ] && export RIAK_PASSWORD="$RIAK_TEST_PASSWORD"
[ -n "${RIAK_TEST_CA:-}" ] && export RIAK_CA="$RIAK_TEST_CA"
[ -n "${RIAK_TEST_TLS_LEGACY:-}" ] && export RIAK_TLS_LEGACY="$RIAK_TEST_TLS_LEGACY"
if [ -n "${RIAK_TEST_FEATURES:-}" ]; then
    export RIAK_FEATURES="$RIAK_TEST_FEATURES"
fi
if [ -n "${RIAK_TEST_PEER_URL:-}" ]; then
    export RIAK_PEER_URL="$RIAK_TEST_PEER_URL"
fi
if [ -n "${RIAK_TEST_TYPE:-}" ]; then
    export RIAK_MULTI_TYPE="$RIAK_TEST_TYPE"
fi
exec sh "$(dirname "$0")/../../examples/cli/run-all.sh"
