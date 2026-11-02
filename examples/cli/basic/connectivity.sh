#!/bin/sh
# Connectivity check: every configured node is pinged, the answering node
# reports its name and version, and the exit codes of the failure cases a
# script has to tell apart are shown.
#   RIAK_URL=pb://10.0.0.1:8087,pb://10.0.0.2:8087 basic/connectivity.sh
set -eu
RIAKCTL=${RIAKCTL:-riakctl}

nodes=$("$RIAKCTL" --compact ping) || {
    echo "no node answers:" >&2
    echo "$nodes" | jq -r '.nodes[] | "  \(.host):\(.port) \(.error // "")"' >&2
    exit 7
}
echo "$nodes" | jq -r '.nodes[] | "\(.host):\(.port)\t\(if .up then "up \(.ms) ms" else "DOWN (\(.error))" end)"'
"$RIAKCTL" --compact info | jq -r '"node \(.node), Riak \(.version), client \(.client_version) over \(.transport)\(if .tls then "+TLS" else "" end)"'

# exit codes: 3 for a missing key, 7 for a node that does not answer
rc=0
"$RIAKCTL" get riakctl-demo "no-such-key-$$" >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 3 ] || { echo "missing key: exit code $rc, expected 3" >&2; exit 1; }
rc=0
url=${RIAK_URL:-pb://127.0.0.1:8087}
"$RIAKCTL" -u "${url%%://*}://127.0.0.1:1" --connect-timeout 1000 ping >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 7 ] || { echo "closed port: exit code $rc, expected 7" >&2; exit 1; }
echo "exit codes: missing key 3, unreachable node 7"
