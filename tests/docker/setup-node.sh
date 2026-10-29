#!/bin/sh
# Prepare a Riak container for the live tests:
#   tests/docker/setup-node.sh <container> [--security <certs-dir>]
# Waits for riak_kv, creates and activates the bucket types the tests use and,
# with --security, installs the TLS certificates of tests/gen_certs.sh, enables
# Riak security and adds the user alice (password "secret").
# Works with Riak 2.x (riak-admin) and 3.x (riak admin).
set -eu
container=${1:?container name}
certs=
if [ "${2:-}" = --security ]; then
    certs=${3:?certificate directory}
fi

x() { docker exec "$container" "$@"; }
# a source build of Riak 3.x lives in /opt/riak (tests/docker/openriak);
# the Riak 2.x packages use riak-admin and /etc/riak
if x test -d /opt/riak/etc; then
    admin() { x riak admin "$@"; }
    etc=/opt/riak/etc
    v3=1
else
    admin() { x riak-admin "$@"; }
    etc=/etc/riak
    v3=
fi

wait_kv() {
    for _ in $(seq 1 90); do
        admin wait-for-service riak_kv >/dev/null 2>&1 && return 0
        sleep 2
    done
    echo "riak_kv did not start" >&2
    return 1
}

create() {
    admin bucket-type create "$1" "$2" >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
        admin bucket-type activate "$1" >/dev/null 2>&1 && return 0
        sleep 2
    done
    echo "cannot activate bucket type $1" >&2
    return 1
}

wait_kv
if admin services 2>/dev/null | grep -q yokozuna; then
    admin wait-for-service yokozuna >/dev/null
fi
create libriak_multi '{"props":{"allow_mult":true}}'
for t in counter set map; do
    create "libriak_${t}s" '{"props":{"datatype":"'"$t"'"}}'
done
# HyperLogLogs need Riak 2.2, grow-only sets Riak 2.9
create libriak_hlls '{"props":{"datatype":"hll"}}' 2>/dev/null ||
    echo "no hll data type on this node" >&2
create libriak_gsets '{"props":{"datatype":"gset"}}' 2>/dev/null ||
    echo "no gset data type on this node" >&2

if [ -n "$certs" ]; then
    docker exec -u 0 "$container" mkdir -p "$etc/certs"
    for f in ca.pem server.pem server.key; do
        docker cp "$certs/$f" "$container:$etc/certs/$f"
    done
    # docker cp keeps the host's owner; the node runs as riak and must read the key
    docker exec -u 0 "$container" chown -R riak:riak "$etc/certs"
    x sh -c "printf 'ssl.certfile = $etc/certs/server.pem\nssl.keyfile = $etc/certs/server.key\nssl.cacertfile = $etc/certs/ca.pem\nlistener.https.internal = 0.0.0.0:8443\n' >> $etc/riak.conf"
    if [ -n "$v3" ]; then
        # Riak 3.x runs in the foreground as the container's process
        docker restart "$container" >/dev/null
    else
        x riak restart >/dev/null
    fi
    wait_kv
    admin security add-user alice password=secret >/dev/null
    admin security add-source all 0.0.0.0/0 password >/dev/null
    # one grant per permission: an unknown permission makes Riak reject the
    # whole grant, and each version knows a different set (Search only where
    # Yokozuna exists, riak_kv.get_preflist from Riak 2.1)
    for perm in riak_kv.get riak_kv.put riak_kv.delete riak_kv.index riak_kv.list_keys \
        riak_kv.list_buckets riak_kv.mapreduce riak_core.get_bucket riak_core.set_bucket \
        riak_core.get_bucket_type riak_core.set_bucket_type riak_kv.get_preflist \
        search.admin search.query; do
        out=$(admin security grant "$perm" on any to alice 2>&1) || true
        case "$out" in
        *[Gg]ranted* | ok*) ;;
        *"not recognized"*) echo "permission $perm not known to this node" >&2 ;;
        *) echo "grant $perm failed: $out" >&2; exit 1 ;;
        esac
    done
    admin security enable >/dev/null
fi
echo "$container ready"
